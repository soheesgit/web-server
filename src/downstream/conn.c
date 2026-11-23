#include "downstream/conn.h"

#include "common/http_framing.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/stub.h"
#include "downstream/request_parser.h"
#include "downstream/response_writer.h"
#include "handler/handler.h"
#include "handler/plan.h"
#include "upstream/relay.h"

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

enum { CONN_IO_SIZE = 8192, CONN_SOCKET_TICK_MS = 100, CONN_DRAIN_MS = 1000 };

struct conn {
    int fd;
    const conn_env_t *env;
    int64_t deadline_ms;
    int64_t header_deadline_ms;
    bool idle;
    bool peer_has_input;
    bool upstream_keep_alive;
    unsigned requests;
    char client_ip[INET6_ADDRSTRLEN];
    uint16_t server_port;
    buf_t input;
    buf_t output;
    buf_t cgi_output;
    response_plan_t plan;
    proxy_job_t job;
};

static int64_t after_ms(int64_t now, int timeout)
{
    if (now < 0 || timeout <= 0) {
        return now;
    }
    return now <= INT64_MAX - timeout ? now + timeout : INT64_MAX;
}

static int64_t earlier(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static int set_client_mode(int fd, bool nonblocking)
{
    if (nonblocking) {
        return platform_set_nonblock(fd);
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return -1;
    }
    const struct timeval timeout = {0, CONN_SOCKET_TICK_MS * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    return 0;
}

static void set_addresses(conn_t *c, const struct sockaddr_storage *peer)
{
    struct sockaddr_storage local;
    socklen_t size = sizeof(local);
    memcpy(c->client_ip, "-", 2);
    if (peer != NULL && peer->ss_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)peer;
        (void)inet_ntop(AF_INET, &address->sin_addr, c->client_ip, sizeof(c->client_ip));
    } else if (peer != NULL && peer->ss_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)peer;
        (void)inet_ntop(AF_INET6, &address->sin6_addr, c->client_ip, sizeof(c->client_ip));
    }
    if (getsockname(c->fd, (struct sockaddr *)&local, &size) == 0) {
        if (local.ss_family == AF_INET) {
            c->server_port = ntohs(((const struct sockaddr_in *)&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            c->server_port = ntohs(((const struct sockaddr_in6 *)&local)->sin6_port);
        }
    }
}

conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env, int64_t now_ms)
{
    size_t max_head = env->lim->max_head;
    size_t cgi_head = env->cfg->cgi_max_head;
    size_t out_head = max_head > cgi_head ? max_head : cgi_head;
    if (out_head > SIZE_MAX - CONN_IO_SIZE - 512 || env->lim->max_body > SIZE_MAX - max_head - 1) {
        errno = EMSGSIZE;
        return NULL;
    }

    conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->fd = -1;
    c->env = env;
    if (buf_init(&c->input, 0, max_head + (size_t)env->lim->max_body + 1) < 0 ||
        buf_init(&c->output, 0, out_head + CONN_IO_SIZE + 512) < 0 ||
        set_client_mode(fd, env->nonblocking) < 0) {
        conn_free(c);
        return NULL;
    }
    plan_init(&c->plan);
    c->fd = fd;
    set_addresses(c, peer);
    c->header_deadline_ms = after_ms(now_ms, env->cfg->header_timeout_ms);
    c->deadline_ms = c->header_deadline_ms;
    return c;
}

void conn_free(conn_t *c)
{
    if (c == NULL) {
        return;
    }
    int saved = errno;
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    buf_cleanup(&c->cgi_output);
    buf_cleanup(&c->output);
    buf_cleanup(&c->input);
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
    errno = saved;
}

static int poll_until(struct pollfd *fds, nfds_t count, int64_t deadline)
{
    for (;;) {
        int64_t now = platform_now_ms();
        if (now < 0 || now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        int64_t remaining = deadline - now;
        int ready = poll(fds, count, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready > 0) {
            return 0;
        }
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static ssize_t receive_request_bytes(conn_t *c, size_t limit)
{
    char data[CONN_IO_SIZE];
    size_t amount = limit < sizeof(data) ? limit : sizeof(data);
    struct pollfd client = {c->fd, POLLIN, 0};
    for (;;) {
        if (poll_until(&client, 1, c->deadline_ms) < 0) {
            return -1;
        }
        ssize_t n = recv(c->fd, data, amount, 0);
        if (n < 0 && (errno == EINTR || would_block())) {
            continue;
        }
        if (n > 0 && buf_append(&c->input, data, (size_t)n) < 0) {
            return -1;
        }
        return n;
    }
}

/* 머리 한도를 채울 때까지 시작 줄에 NUL이 있으면 HTTP 응답 없이 종료한다 (E-ER-12). */
static bool binary_start_line(const buf_t *input)
{
    const char *newline = memchr(input->data, '\n', input->len);
    size_t len = newline == NULL ? input->len : (size_t)(newline - input->data);
    return memchr(input->data, '\0', len) != NULL;
}

/* 0: 요청 완료, -1: 응답 없이 종료, 양수: 오류 응답의 HTTP 상태. */
static int receive_request(conn_t *c, http_request_t *req, size_t *out_size)
{
    int status = 500;
    size_t limit = c->env->lim->max_head + 1;
    for (;;) {
        if (c->input.len != 0) {
            http_parse_result_t result =
                request_parse_head(c->input.data, c->input.len, c->env->lim, req, &status);
            if (result == HTTP_PARSE_DONE) {
                break;
            }
            if (result == HTTP_PARSE_ERROR) {
                return status == 431 && binary_start_line(&c->input) ? -1 : status;
            }
        }
        if (c->input.len >= limit) {
            return -1;
        }
        ssize_t n = receive_request_bytes(c, limit - c->input.len);
        if (n <= 0) {
            return n < 0 && errno == ETIMEDOUT && c->input.len != 0 ? 408 : -1;
        }
        if (c->idle) {
            c->idle = false;
            c->deadline_ms = c->header_deadline_ms;
            if (platform_now_ms() >= c->deadline_ms) {
                return 408;
            }
        }
    }

    size_t head_len = req->head_len;
    size_t body_len = req->framing.kind == FRAMING_LENGTH ? (size_t)req->framing.length : 0;
    size_t total = head_len + body_len;
    if (c->input.len < total) {
        c->deadline_ms = after_ms(platform_now_ms(), c->env->cfg->body_timeout_ms);
        while (c->input.len < total) {
            ssize_t n = receive_request_bytes(c, total - c->input.len);
            if (n <= 0) {
                return n < 0 && errno == ETIMEDOUT ? 408 : -1;
            }
        }
        /* 본문을 받는 동안 수신 버퍼가 이동했을 수 있으므로 빌린 뷰를 다시 만든다. */
        if (request_parse_head(c->input.data, head_len, c->env->lim, req, &status) !=
            HTTP_PARSE_DONE) {
            return 500;
        }
    }
    if (body_len != 0) {
        req->body = (str_t){c->input.data + head_len, body_len};
    }
    *out_size = total;
    return 0;
}

static void extend_send_deadline(conn_t *c)
{
    c->deadline_ms = after_ms(platform_now_ms(), c->env->cfg->body_timeout_ms);
}

static int wait_to_send(conn_t *c)
{
    int64_t deadline = c->deadline_ms;
    if (c->plan.kind == BODY_PIPE) {
        deadline = earlier(deadline, c->plan.body.pipe.deadline_ms);
    }
    struct pollfd client = {c->fd, POLLOUT, 0};
    return poll_until(&client, 1, deadline);
}

static int send_bytes(conn_t *c, const char *data, size_t len, uint64_t *sent)
{
    size_t offset = 0;
    while (offset < len) {
        if (wait_to_send(c) < 0) {
            return -1;
        }
        size_t left = len - offset;
        ssize_t n = platform_send(c->fd, data + offset, left < CONN_IO_SIZE ? left : CONN_IO_SIZE);
        if (n < 0 && would_block()) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        offset += (size_t)n;
        if (sent != NULL) {
            *sent += (uint64_t)n;
        }
        extend_send_deadline(c);
    }
    return 0;
}

static int send_file(conn_t *c, uint64_t *sent)
{
    off_t offset = c->plan.body.file.off;
    off_t remaining = c->plan.body.file.len;
    while (remaining > 0) {
        if (wait_to_send(c) < 0) {
            return -1;
        }
        off_t previous = offset;
        size_t count = remaining < CONN_IO_SIZE ? (size_t)remaining : CONN_IO_SIZE;
        ssize_t n = platform_sendfile(c->fd, c->plan.body.file.fd, &offset, count);
        int send_error = errno;
        off_t progress = offset - previous;
        if (progress > 0) {
            remaining -= progress;
            *sent += (uint64_t)progress;
            extend_send_deadline(c);
        }
        if (n == 0 || (n < 0 && send_error != EAGAIN && send_error != EWOULDBLOCK)) {
            return -1;
        }
    }
    return 0;
}

/* 요청 여분은 수신 버퍼에 남겨야 한다. 이미 읽을 바이트가 있으면 HUP/ERR만 감시한다. */
static struct pollfd watch_client(const conn_t *c)
{
    return (struct pollfd){c->fd, c->peer_has_input ? 0 : POLLIN, 0};
}

static bool client_disconnected(conn_t *c, short events)
{
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = ECONNRESET;
        return true;
    }
    if (events & POLLIN) {
        char byte;
        ssize_t n = recv(c->fd, &byte, 1, MSG_PEEK);
        if (n == 0 || (n < 0 && errno != EINTR && !would_block())) {
            errno = ECONNRESET;
            return true;
        }
        c->peer_has_input = n > 0;
    }
    return false;
}

static void close_cgi_stdin(response_plan_t *plan)
{
    if (plan->body.pipe.in_fd >= 0) {
        close(plan->body.pipe.in_fd);
        plan->body.pipe.in_fd = -1;
    }
}

static int pump_cgi_stdin(response_plan_t *plan)
{
    buf_t *input = &plan->body.pipe.in_data;
    size_t size = input->len < CONN_IO_SIZE ? input->len : CONN_IO_SIZE;
    ssize_t n = write(plan->body.pipe.in_fd, input->data, size);
    if (n > 0) {
        buf_consume(input, (size_t)n);
        if (input->len == 0) {
            close_cgi_stdin(plan);
        }
    } else if (n < 0 && errno == EPIPE) {
        close_cgi_stdin(plan);
    } else if (n == 0 || (errno != EINTR && !would_block())) {
        return -1;
    }
    return 0;
}

static ssize_t read_cgi(conn_t *c, char *data, size_t size)
{
    response_plan_t *p = &c->plan;
    if (p->body.pipe.in_data.len == 0) {
        close_cgi_stdin(p);
    }
    for (;;) {
        struct pollfd fds[] = {
            watch_client(c),
            {p->body.pipe.out_fd, POLLIN, 0},
            {p->body.pipe.in_fd, POLLOUT, 0},
        };
        int64_t deadline = earlier(c->deadline_ms, p->body.pipe.deadline_ms);
        if (poll_until(fds, 3, deadline) < 0 || client_disconnected(c, fds[0].revents)) {
            return -1;
        }
        if (fds[2].revents != 0 && pump_cgi_stdin(p) < 0) {
            return -1;
        }
        if (fds[1].revents != 0) {
            ssize_t n = read(p->body.pipe.out_fd, data, size);
            if (n >= 0 || (errno != EINTR && !would_block())) {
                return n;
            }
        }
    }
}

static bool cgi_failed(response_plan_t *p)
{
    if (p->body.pipe.pid <= 0) {
        return false;
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(p->body.pipe.pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0) {
        return false;
    }
    if (waited > 0 || errno == ECHILD) {
        p->body.pipe.pid = 0;
    }
    return waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

static int prepare_cgi(conn_t *c, int client_minor)
{
    response_plan_t *p = &c->plan;
    c->deadline_ms = p->body.pipe.deadline_ms;
    if (platform_set_nonblock(p->body.pipe.out_fd) < 0 ||
        (p->body.pipe.in_fd >= 0 && platform_set_nonblock(p->body.pipe.in_fd) < 0) ||
        buf_init(&c->cgi_output, 0, (size_t)c->env->cfg->cgi_max_head + CONN_IO_SIZE) < 0) {
        return 500;
    }

    char data[CONN_IO_SIZE];
    for (;;) {
        ssize_t n = read_cgi(c, data, sizeof(data));
        if (n <= 0) {
            if (n < 0 && errno == ECONNRESET) {
                return -1;
            }
            return n < 0 && errno == ETIMEDOUT ? 504 : 502;
        }
        if (buf_append(&c->cgi_output, data, (size_t)n) < 0) {
            return 502;
        }

        size_t used;
        int status = 502;
        http_parse_result_t parsed =
            cgi_parse_head(c->cgi_output.data, c->cgi_output.len, c->env->cfg->cgi_max_head,
                           &p->head, &used, &status);
        if (parsed == HTTP_PARSE_ERROR) {
            return status;
        }
        if (parsed == HTTP_PARSE_DONE) {
            if (cgi_failed(p)) {
                return 502;
            }
            if (platform_now_ms() >= p->body.pipe.deadline_ms) {
                return 504;
            }
            p->head_pending = false;
            buf_consume(&c->cgi_output, used);
            if (p->head.framing.kind == FRAMING_NONE) {
                p->head.framing.kind = client_minor == 1 ? FRAMING_CHUNKED : FRAMING_CLOSE;
            }
            return 0;
        }
    }
}

static int send_pipe(conn_t *c, uint64_t *sent)
{
    http_framing_t framing = c->plan.head.framing;
    uint64_t remaining = framing.length;
    char block[CONN_IO_SIZE];
    const char *data = c->cgi_output.data;
    size_t size = c->cgi_output.len;
    for (;;) {
        if (framing.kind == FRAMING_LENGTH && size > remaining) {
            size = (size_t)remaining;
        }
        if (size != 0) {
            if (framing.kind == FRAMING_CHUNKED) {
                buf_reset(&c->output);
                if (response_write_chunk(data, size, &c->output) < 0 ||
                    send_bytes(c, c->output.data, c->output.len, NULL) < 0) {
                    return -1;
                }
                *sent += size;
            } else if (send_bytes(c, data, size, sent) < 0) {
                return -1;
            }
        }
        if (framing.kind == FRAMING_LENGTH) {
            remaining -= size;
            if (remaining == 0) {
                return 0;
            }
        }
        ssize_t n = read_cgi(c, block, sizeof(block));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            if (framing.kind == FRAMING_LENGTH || cgi_failed(&c->plan)) {
                return -1;
            }
            if (framing.kind == FRAMING_CHUNKED) {
                buf_reset(&c->output);
                if (response_write_last_chunk(&c->output) < 0) {
                    return -1;
                }
                return send_bytes(c, c->output.data, c->output.len, NULL);
            }
            return 0;
        }
        data = block;
        size = (size_t)n;
    }
}

static int prepare_upstream(conn_t *c, int client_minor)
{
    for (;;) {
        upstream_conn_t *up = NULL;
        http_response_t resp;
        proxy_fail_t why = PFAIL_CONNECT;
        const config_t *cfg = c->env->cfg;
        if (relay_exchange_blocking(&c->job, c->env->lim, cfg->upstream_connect_timeout_ms,
                                    cfg->upstream_response_timeout_ms, &up, &resp,
                                    &why) == RELAY_OK) {
            if (client_minor == 0 && resp.framing.kind == FRAMING_CHUNKED) {
                upstream_release(up, false);
                return plan_error(&c->plan, 502);
            }
            if (proxy_on_head(&c->job, up, &resp, &c->plan) == 0) {
                buf_consume(&up->rbuf, resp.head_len);
                c->upstream_keep_alive = resp.keep_alive;
            }
            return 0;
        }
        if (proxy_on_fail(&c->job, why, &c->plan) == PROXY_GIVE_UP) {
            return 0;
        }
    }
}

static ssize_t read_upstream(conn_t *c, int fd, char *data, size_t size)
{
    for (;;) {
        struct pollfd fds[] = {watch_client(c), {fd, POLLIN, 0}};
        if (poll_until(fds, 2, c->deadline_ms) < 0 || client_disconnected(c, fds[0].revents)) {
            return -1;
        }
        if (fds[1].revents != 0) {
            ssize_t n = recv(fd, data, size, 0);
            if (n >= 0 || (errno != EINTR && !would_block())) {
                return n;
            }
        }
    }
}

static void finish_upstream(conn_t *c, bool complete)
{
    upstream_release(c->plan.body.upstream.conn, complete && c->upstream_keep_alive);
    c->plan.body.upstream.conn = NULL;
}

static int send_upstream(conn_t *c, uint64_t *sent)
{
    upstream_conn_t *up = c->plan.body.upstream.conn;
    http_body_tracker_t *tracker = &c->plan.body.upstream.tracker;
    char block[CONN_IO_SIZE];
    const char *data = up->rbuf.data;
    size_t size = up->rbuf.len;
    for (;;) {
        size_t used = 0;
        http_body_result_t result = http_body_track(tracker, data, size, &used);
        if (result == BODY_ERROR || send_bytes(c, data, used, sent) < 0) {
            return -1;
        }
        if (result == BODY_DONE) {
            finish_upstream(c, used == size);
            return 0;
        }

        ssize_t n = read_upstream(c, up->fd, block, sizeof(block));
        if (n <= 0) {
            if (n == 0 && tracker->f.kind == FRAMING_CLOSE) {
                finish_upstream(c, false);
                return 0;
            }
            return -1;
        }
        data = block;
        size = (size_t)n;
    }
}

static int prepare_response(conn_t *c, const http_request_t *req, time_t now)
{
    const route_t *route = router_match(c->env->router, req->host, req->path);
    if (route == NULL) {
        return plan_error(&c->plan, 404);
    }
    handler_ctx_t ctx = {
        .cfg = c->env->cfg,
        .lim = c->env->lim,
        .client_ip = {c->client_ip, strlen(c->client_ip)},
        .server_port = c->server_port,
        .now = now,
        .now_ms = platform_now_ms(),
    };
    if (handler_dispatch(req, route, &ctx, &c->job, &c->plan) == HANDLER_NEED_UPSTREAM) {
        return prepare_upstream(c, req->minor);
    }
    if (c->plan.kind == BODY_PIPE && c->plan.body.pipe.deadline_ms < 0) {
        c->plan.body.pipe.deadline_ms = after_ms(ctx.now_ms, c->env->cfg->cgi_timeout_ms);
    }
    if (c->plan.kind == BODY_PIPE && c->plan.head_pending) {
        int status = prepare_cgi(c, req->minor);
        return status > 0 ? plan_error(&c->plan, status) : status;
    }
    return 0;
}

static int send_body(conn_t *c, bool head_request, uint64_t *sent)
{
    const response_plan_t *p = &c->plan;
    int status = p->head.status;
    if (head_request || status < 200 || status == 204 || status == 304) {
        if (p->kind == BODY_UPSTREAM) {
            finish_upstream(c, p->body.upstream.tracker.f.kind == FRAMING_NONE &&
                                   p->body.upstream.conn->rbuf.len == 0);
        }
        return 0;
    }
    switch (p->kind) {
    case BODY_NONE:
        return 0;
    case BODY_MEM:
        return send_bytes(c, p->body.mem.data.data, p->body.mem.data.len, sent);
    case BODY_FILE:
        return send_file(c, sent);
    case BODY_PIPE:
        return send_pipe(c, sent);
    case BODY_UPSTREAM:
        return send_upstream(c, sent);
    }
    return -1;
}

static void drain_on_close(conn_t *c)
{
    if (shutdown(c->fd, SHUT_WR) < 0) {
        return;
    }
    int64_t deadline = after_ms(platform_now_ms(), CONN_DRAIN_MS);
    char discard[CONN_IO_SIZE];
    struct pollfd fd = {c->fd, POLLIN, 0};
    while (poll_until(&fd, 1, deadline) == 0) {
        ssize_t n = recv(c->fd, discard, sizeof(discard), 0);
        if (n == 0 || (n < 0 && errno != EINTR && !would_block())) {
            return;
        }
    }
}

static void finish_request(conn_t *c)
{
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    plan_init(&c->plan);
    buf_cleanup(&c->cgi_output);
    c->upstream_keep_alive = false;
    c->peer_has_input = false;
}

static void log_request(conn_t *c, const http_request_t *req, int status, uint64_t sent,
                        int64_t started)
{
    const char *method = NULL;
    if (req != NULL) {
        switch (req->method) {
        case HTTP_GET:
            method = "GET";
            break;
        case HTTP_HEAD:
            method = "HEAD";
            break;
        case HTTP_POST:
            method = "POST";
            break;
        case HTTP_OTHER:
            break;
        }
    }
    char *target = NULL;
    if (req != NULL) {
        target = malloc(req->target.len + 1);
        if (target != NULL) {
            memcpy(target, req->target.ptr, req->target.len);
            target[req->target.len] = '\0';
        }
    }
    log_access(c->client_ip, method, target, status, sent, platform_now_ms() - started);
    free(target);
}

void conn_serve_blocking(conn_t *c)
{
    if (c->env->nonblocking) {
        return;
    }

    for (;;) {
        http_request_t req = {0};
        size_t request_size;
        int64_t started = platform_now_ms();
        int parse_error = receive_request(c, &req, &request_size);
        if (parse_error < 0) {
            break;
        }

        time_t now = platform_wall_time();
        int prepared =
            parse_error ? plan_error(&c->plan, parse_error) : prepare_response(c, &req, now);
        if (prepared != 0) {
            finish_request(c);
            break;
        }

        const config_t *cfg = c->env->cfg;
        http_head_out_t *head = &c->plan.head;
        c->requests++;
        bool close_after = parse_error != 0 || !req.keep_alive || head->close ||
                           head->framing.kind == FRAMING_CLOSE ||
                           (req.minor == 0 && head->framing.kind == FRAMING_CHUNKED) ||
                           (cfg->max_keepalive_requests > 0 &&
                            c->requests >= (unsigned)cfg->max_keepalive_requests);
        head->close = close_after;
        buf_reset(&c->output);
        extend_send_deadline(c);
        uint64_t sent = 0;
        int status = head->status;
        bool sent_head = response_write_head(head, req.minor, now, &c->output) == 0 &&
                         send_bytes(c, c->output.data, c->output.len, NULL) == 0;
        bool sent_all =
            sent_head && send_body(c, !parse_error && req.method == HTTP_HEAD, &sent) == 0;
        log_request(c, parse_error ? NULL : &req, status, sent, started);
        finish_request(c);
        if (sent_all && parse_error != 0) {
            drain_on_close(c);
        }
        if (!sent_all || close_after) {
            break;
        }

        buf_consume(&c->input, request_size);
        int64_t done = platform_now_ms();
        c->header_deadline_ms = after_ms(done, cfg->header_timeout_ms);
        c->idle = c->input.len == 0;
        c->deadline_ms =
            c->idle ? after_ms(done, cfg->keepalive_timeout_ms) : c->header_deadline_ms;
    }
    c->deadline_ms = -1;
}

conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(now_ms);
    /* 이벤트 경로는 7단계에서 구현한다. */
    STUB_RETURN(CONN_CLOSE);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(now_ms);
    /* v1의 마감은 blocking 루프에서 처리한다. */
    STUB_RETURN(CONN_CLOSE);
}

int conn_fd(const conn_t *c)
{
    return c->fd;
}

int64_t conn_deadline_ms(const conn_t *c)
{
    return c->deadline_ms;
}
