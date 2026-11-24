/* src/downstream/conn.c
 *
 * 연결 상태 머신. 명세: conn.h
 * v1은 연결을 blocking으로, v2는 이벤트마다 제한된 작업량만 처리한다.
 */
#include "downstream/conn.h"

#include "common/http_framing.h"
#include "core/log.h"
#include "core/platform.h"
#include "downstream/internal_response.h"
#include "downstream/request_parser.h"
#include "downstream/response_writer.h"
#include "handler/handler.h"
#include "handler/plan.h"
#include "upstream/relay.h"
#include "upstream/request_writer.h"
#include "upstream/upstream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CONN_TICK_MS 200 /* v1 SO_RCVTIMEO·SO_SNDTIMEO 주기. 이 주기마다 마감을 확인 */
#define CONN_DRAIN_MS 1000       /* 에러 응답 뒤 남은 입력을 버리는 최대 시간 */
#define CONN_IO_CHUNK 16384      /* pipe·업스트림 한 번 읽기 크기 */
#define CONN_WBUF_MAX (1u << 20) /* 송신 버퍼 상한 (머리 + 작은 메모리 본문) */
#define CONN_LOG_TARGET_MAX 8192 /* 접근 로그에 남기는 target 최대 길이 */

#define CONN_EVENT_STEPS 32

typedef enum { CHUNK_SIZE_LINE, CHUNK_PAYLOAD, CHUNK_SEPARATOR, CHUNK_TRAILERS } chunk_part_t;

typedef struct {
    chunk_part_t part;
    size_t metadata;
} chunk_relay_t;

typedef enum {
    NB_HEAD,
    NB_REQUEST_BODY,
    NB_RELAY_HEAD,
    NB_CGI_HEAD,
    NB_SEND,
    NB_DRAIN,
    NB_CLOSE,
} nb_phase_t;

typedef struct {
    nb_phase_t phase;
    conn_status_t waiting;
    int source_fd, cgi_in_fd;
    unsigned source_events;
    int64_t deadline, started;
    size_t write_off, cgi_in_off, body_off;
    uint64_t sent, flushed;
    bool close_after, parser_error, pending_input, body_done, with_body;
    bool log_response, stream_failed;
    int minor;
    http_request_t req;
    route_t route;
    relay_t *relay;
    chunk_relay_t chunk;
} conn_event_t;

struct conn {
    conn_event_t *event; /* v2 전용 진행 정보 */
    int fd;
    const conn_env_t *env;
    char ip[INET6_ADDRSTRLEN]; /* 상대 주소 문자열, 모르면 "-" */
    uint16_t server_port;      /* 이 연결의 로컬 포트 (CGI SERVER_PORT) */
    buf_t rbuf;                /* 클라이언트 수신 버퍼 */
    buf_t wbuf;                /* 클라이언트 송신 버퍼 */
    buf_t cgi_out;             /* CGI 출력 머리 수집 */
    int64_t head_deadline_ms;  /* 헤더 수신 절대 마감 */
    int64_t idle_deadline_ms;  /* 요청 사이 유휴 마감. 첫 요청 전은 -1 */
    int n_requests;            /* 이 연결에서 처리한 요청 수 */
    response_plan_t plan;      /* 현재 요청의 계획 */
    proxy_job_t job;           /* 현재 요청의 프록시 작업 */
    size_t up_head_len;        /* BODY_UPSTREAM: uc->rbuf 앞의 응답 머리 길이 */
    uint16_t forward_port;     /* 로컬 리스너의 absolute-form 프록시 사용 */
    bool client_canceled;      /* 클라이언트가 업스트림 대기 중 종료 */
    bool up_reusable; /* BODY_UPSTREAM: 업스트림 응답이 연결 재사용을 허용 */
};

typedef enum { IO_OK, IO_EOF, IO_TIMEOUT, IO_ERR } io_result_t;

/* ── 작은 도우미 ─────────────────────────────────────────────────────────── */

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

/* s를 out에 NUL 종료로 복사한다. 자리가 모자라면 자른다. */
static void copy_cstr(str_t s, char *out, size_t cap)
{
    size_t n = s.len < cap - 1 ? s.len : cap - 1;

    if (n > 0) {
        memcpy(out, s.ptr, n);
    }
    out[n] = '\0';
}

static void format_ip(const struct sockaddr_storage *peer, char *out, size_t cap)
{
    const void *addr = NULL;

    if (peer != NULL && peer->ss_family == AF_INET) {
        addr = &((const struct sockaddr_in *)(const void *)peer)->sin_addr;
    } else if (peer != NULL && peer->ss_family == AF_INET6) {
        addr = &((const struct sockaddr_in6 *)(const void *)peer)->sin6_addr;
    }
    if (addr == NULL || inet_ntop(peer->ss_family, addr, out, (socklen_t)cap) == NULL) {
        snprintf(out, cap, "-");
    }
}

static uint16_t local_port(int fd)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);

    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(((const struct sockaddr_in *)(const void *)&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((const struct sockaddr_in6 *)(const void *)&ss)->sin6_port);
    }
    return 0;
}

/* D-012: v2면 O_NONBLOCK, v1이면 blocking + 짧은 SO_RCVTIMEO·SO_SNDTIMEO. */
static int set_mode(int fd, bool nonblocking)
{
    struct timeval tv = {0, CONN_TICK_MS * 1000};
    int fl;

    if (nonblocking) {
        return platform_set_nonblock(fd);
    }
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
}

/* poll을 마감까지 기다린다. >0 준비됨, 0 마감, -1 오류. */
static int poll_until(struct pollfd *pfd, nfds_t n, int64_t deadline_ms)
{
    for (;;) {
        int64_t left = deadline_ms - platform_now_ms();
        int r;

        if (left <= 0) {
            return 0;
        }
        r = poll(pfd, n, (int)min64(left, INT_MAX));
        if (r > 0 || (r < 0 && errno != EINTR)) {
            return r;
        }
    }
}

/* 클라이언트 입력을 소비하지 않는다. 파이프라인 입력은 다음 요청을 위해 보존한다. */
static bool client_gone(const conn_t *c, short events)
{
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return true;
    }
    char ch;
    ssize_t n = recv(c->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

/* 클라이언트 취소와 업스트림 진척 중 먼저 도착하는 일을 처리한다. */
static int proxy_wait(conn_t *c, int fd, short events, int64_t deadline)
{
    struct pollfd pfd[2] = {{fd, events, 0}, {c->fd, POLLIN, 0}};
    for (;;) {
        int rc = poll_until(pfd, 2, deadline);
        if (rc <= 0) {
            return rc;
        }
        if (pfd[1].revents != 0) {
            if (client_gone(c, pfd[1].revents)) {
                return -2;
            }
            /* 이미 읽을 파이프라인이 있으면 POLLIN으로 계속 깨지 않게 한다. */
            pfd[1].events = 0;
        }
        if (pfd[0].revents != 0) {
            return 1;
        }
    }
}

/* ── 클라이언트 I/O (v1 blocking, SO_RCVTIMEO 주기로 깨어나 마감 확인) ───────── */

static io_result_t read_more(conn_t *c, int64_t deadline_ms)
{
    for (;;) {
        ssize_t n;

        if (platform_now_ms() >= deadline_ms) {
            return IO_TIMEOUT;
        }
        n = buf_read_fd(&c->rbuf, c->fd);
        if (n > 0) {
            return IO_OK;
        }
        if (n == 0) {
            return IO_EOF;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return IO_ERR;
        }
    }
}

/* n바이트를 모두 보낸다. 진행 없이 body_timeout_ms가 지나면 실패. */
static int send_all(conn_t *c, const char *p, size_t n)
{
    int64_t stall = platform_now_ms() + c->env->cfg->body_timeout_ms;

    while (n > 0) {
        ssize_t w = platform_send(c->fd, p, n);

        if (w > 0) {
            p += w;
            n -= (size_t)w;
            stall = platform_now_ms() + c->env->cfg->body_timeout_ms;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        if (platform_now_ms() >= stall) {
            return -1;
        }
    }
    return 0;
}

static int flush_wbuf(conn_t *c)
{
    int rc = send_all(c, c->wbuf.data, c->wbuf.len);

    buf_reset(&c->wbuf);
    return rc;
}

/* 본문 조각 하나를 framing에 맞게 보낸다 (CHUNKED면 청크로 감쌈). */
static int send_body_piece(conn_t *c, const char *p, size_t n, bool chunked)
{
    if (n == 0) {
        return 0;
    }
    if (!chunked) {
        return c->event != NULL ? buf_append(&c->wbuf, p, n) : send_all(c, p, n);
    }
    if (response_write_chunk(p, n, &c->wbuf) != 0) {
        return -1;
    }
    return c->event != NULL ? 0 : flush_wbuf(c);
}

static int send_file(conn_t *c, int fd, off_t off, off_t len, uint64_t *sent)
{
    off_t end = off + len;
    int64_t stall = platform_now_ms() + c->env->cfg->body_timeout_ms;

    while (off < end) {
        off_t before = off;
        ssize_t w = platform_sendfile(c->fd, fd, &off, (size_t)(end - off));

        if (off > before) {
            *sent += (uint64_t)(off - before);
            stall = platform_now_ms() + c->env->cfg->body_timeout_ms;
        }
        if (w == 0 && off == before) {
            return -1; /* 파일이 줄었다. 약속한 길이를 채울 수 없다 */
        }
        if (w < 0 && ((errno != EAGAIN && errno != EWOULDBLOCK) || platform_now_ms() >= stall)) {
            return -1;
        }
    }
    return 0;
}

/* 에러 응답 뒤: 쓰기 쪽을 닫고 남은 입력을 잠깐 버려 RST로 응답이 유실되지 않게 한다. */
static void drain_input(conn_t *c)
{
    int64_t until = platform_now_ms() + CONN_DRAIN_MS;
    char tmp[4096];

    shutdown(c->fd, SHUT_WR);
    while (platform_now_ms() < until) {
        ssize_t n = read(c->fd, tmp, sizeof(tmp));

        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            break;
        }
    }
}

/* ── 응답 머리 + 본문 ─────────────────────────────────────────────────────── */

/* 머리를 쓴다. 메모리 본문은 머리와 함께 한 번에 보낸다. */
static int send_head_and_mem(conn_t *c, int minor, bool with_body, uint64_t *sent)
{
    const response_plan_t *p = &c->plan;
    bool mem = with_body && p->kind == BODY_MEM && p->body.mem.data.len > 0;

    buf_reset(&c->wbuf);
    if (response_write_head(&p->head, minor, platform_wall_time(), &c->wbuf) != 0) {
        return -1;
    }
    if (mem && buf_append(&c->wbuf, p->body.mem.data.data, p->body.mem.data.len) == 0) {
        *sent += p->body.mem.data.len;
        return flush_wbuf(c);
    }
    if (flush_wbuf(c) != 0) {
        return -1;
    }
    if (mem) {
        if (send_all(c, p->body.mem.data.data, p->body.mem.data.len) != 0) {
            return -1;
        }
        *sent += p->body.mem.data.len;
    }
    return 0;
}

/* 파서 오류·408: 에러 응답을 보내고 연결을 닫는다. 항상 false (닫음). */
static bool respond_error(conn_t *c, int status, int minor)
{
    uint64_t sent = 0;

    plan_cleanup(&c->plan);
    plan_init(&c->plan);
    if (plan_error(&c->plan, status) == 0) {
        c->plan.head.close = true;
        (void)send_head_and_mem(c, minor, true, &sent);
    }
    log_access(c->ip, "-", "-", status, sent, 0);
    plan_cleanup(&c->plan);
    drain_input(c);
    return false;
}

/* ── CGI (BODY_PIPE) ─────────────────────────────────────────────────────── */

/* 자식 stdin에 쓸 것이 있으면 쓰고, 다 쓰면 닫는다. 자식이 stdin을 닫았으면 그만 쓴다. */
static void cgi_pump_stdin(response_plan_t *p)
{
    ssize_t w;

    if (p->body.pipe.in_fd < 0) {
        return;
    }
    w = buf_write_fd(&p->body.pipe.in_data, p->body.pipe.in_fd);
    if ((w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || p->body.pipe.in_data.len == 0) {
        close(p->body.pipe.in_fd);
        p->body.pipe.in_fd = -1;
    }
}

/* stdout이 읽을 수 있게 될 때까지 기다리며 그동안 stdin을 쓴다. 1 준비, 0 마감, -1 오류. */
static int cgi_wait_out(conn_t *c)
{
    response_plan_t *p = &c->plan;
    short client_events = POLLIN;
    int64_t deadline = p->body.pipe.deadline_ms;

    for (;;) {
        struct pollfd pfd[] = {{p->body.pipe.out_fd, POLLIN, 0},
                               {p->body.pipe.in_fd, POLLOUT, 0},
                               {c->fd, client_events, 0}};
        int r = poll_until(pfd, 3, deadline < 0 ? INT64_MAX : deadline);

        if (r <= 0) {
            return r;
        }
        if (pfd[2].revents != 0) {
            if (client_gone(c, pfd[2].revents)) {
                return -2;
            }
            client_events = 0;
        }
        if ((pfd[1].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            cgi_pump_stdin(p);
        }
        if ((pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            return 1;
        }
    }
}

/* D-029: 길이를 모르는 CGI 응답은 1.1에 청크, 1.0에 EOF 경계를 쓴다. */
static void take_cgi_head(response_plan_t *p, http_head_out_t *h, int minor)
{
    if (response_status_has_no_body(h->status)) {
        h->framing = (http_framing_t){FRAMING_NONE, 0};
    } else if (h->framing.kind == FRAMING_NONE) {
        h->framing.kind = minor >= 1 ? FRAMING_CHUNKED : FRAMING_CLOSE;
    }
    h->close = p->head.close;
    buf_cleanup(&p->head.fields);
    p->head = *h;
    p->head_pending = false;
}

/* CGI 출력에서 응답 머리를 확정한다. 실패하면 plan을 502·504 에러 계획으로 바꾼다.
 * 성공하면 cgi_out의 [*out_body_off, len)이 본문 앞부분이다. */
static void cgi_resolve_head(conn_t *c, int minor, size_t *out_body_off)
{
    response_plan_t *p = &c->plan;
    const config_t *cfg = c->env->cfg;

    *out_body_off = 0;
    if (buf_init(&c->cgi_out, 4096, cfg->cgi_max_head + CONN_IO_CHUNK) != 0 ||
        platform_set_nonblock(p->body.pipe.out_fd) != 0 ||
        (p->body.pipe.in_fd >= 0 && platform_set_nonblock(p->body.pipe.in_fd) != 0)) {
        plan_error(p, 500);
        return;
    }
    if (p->body.pipe.in_data.len == 0 && p->body.pipe.in_fd >= 0) {
        close(p->body.pipe.in_fd);
        p->body.pipe.in_fd = -1;
    }
    for (;;) {
        http_head_out_t h;
        size_t used = 0;
        int st = 0;
        http_parse_result_t r;
        ssize_t n;
        int w;

        memset(&h, 0, sizeof(h));
        r = cgi_parse_head(c->cgi_out.data, c->cgi_out.len, cfg->cgi_max_head, &h, &used, &st);
        if (r == HTTP_PARSE_DONE) {
            take_cgi_head(p, &h, minor);
            *out_body_off = used;
            return;
        }
        buf_cleanup(&h.fields);
        if (r == HTTP_PARSE_ERROR) {
            plan_error(p, 502);
            return;
        }
        w = cgi_wait_out(c);
        if (w == -2) {
            plan_cleanup(p);
            c->client_canceled = true;
            return;
        }
        if (w == 0) {
            plan_error(p, 504); /* plan_cleanup이 자식을 SIGKILL·waitpid */
            return;
        }
        n = w < 0 ? -1 : buf_read_fd(&c->cgi_out, p->body.pipe.out_fd);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            plan_error(p, 502); /* 머리 전에 EOF 또는 오류 */
            return;
        }
    }
}

/* CGI 본문을 EOF(또는 Content-Length)까지 framing대로 보낸다. */
static int cgi_send_body(conn_t *c, const char *pre, size_t pre_len, uint64_t *sent)
{
    response_plan_t *p = &c->plan;
    bool chunked = p->head.framing.kind == FRAMING_CHUNKED;
    uint64_t limit = p->head.framing.kind == FRAMING_LENGTH ? p->head.framing.length : UINT64_MAX;
    char tmp[CONN_IO_CHUNK];

    if (pre_len > limit) {
        pre_len = (size_t)limit;
    }
    if (send_body_piece(c, pre, pre_len, chunked) != 0) {
        return -1;
    }
    *sent += pre_len;
    while (*sent < limit) {
        int w = cgi_wait_out(c);
        ssize_t n;

        if (w <= 0) {
            return -1; /* 머리를 보낸 뒤의 마감·오류는 연결을 닫는다 */
        }
        n = read(p->body.pipe.out_fd, tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((uint64_t)n > limit - *sent) {
            n = (ssize_t)(limit - *sent);
        }
        if (send_body_piece(c, tmp, (size_t)n, chunked) != 0) {
            return -1;
        }
        *sent += (uint64_t)n;
    }
    if (limit != UINT64_MAX && *sent < limit) {
        return -1; /* Content-Length보다 짧게 끝났다 */
    }
    if (chunked) {
        if (response_write_last_chunk(&c->wbuf) != 0) {
            return -1;
        }
        return flush_wbuf(c);
    }
    return 0;
}

/* ── 프록시 (BODY_UPSTREAM) ─────────────────────────────────────────────── */

static int write_proxy_request(conn_t *c, const http_request_t *req, const route_t *rt)
{
    upstream_rewrite_t rw = {
        .client_ip = str_from_cstr(c->ip),
        .upstream_host = str_from_cstr(backend_hostport(c->job.backend)),
        .keep_host = rt->keep_host,
    };
    if (rt->strip_prefix) {
        rw.strip_prefix = (str_t){rt->prefix, rt->prefix_len};
        if (rw.strip_prefix.len > 0 && rw.strip_prefix.ptr[rw.strip_prefix.len - 1] == '/') {
            rw.strip_prefix.len--;
        }
    }
    return request_write_upstream(req, &rw, &c->job.request);
}

static relay_status_t drive_relay(conn_t *c, relay_t *relay, bool *out_canceled)
{
    unsigned events = 0;
    for (;;) {
        relay_status_t status = relay_step(relay, events, platform_now_ms());
        if (status != RELAY_WANT_READ && status != RELAY_WANT_WRITE) {
            return status;
        }
        bool read = status == RELAY_WANT_READ;
        int ready =
            proxy_wait(c, relay_fd(relay), read ? POLLIN : POLLOUT, relay_deadline_ms(relay));
        if (ready < 0) {
            *out_canceled = ready == -2;
            return RELAY_FAIL;
        }
        events = ready > 0 ? (read ? EV_READ : EV_WRITE) : 0;
    }
}

static int setup_proxy_job(conn_t *c, const http_request_t *req, const route_t *rt)
{
    c->job = (proxy_job_t){
        .group = rt->upstream,
        .method = req->method,
        .attempts = 1,
        .max_attempts = (int)upstream_backend_count(rt->upstream),
        .client_minor = req->minor,
        .client_keep_alive = req->keep_alive,
    };
    if (req->body.len > SIZE_MAX - req->head_len - 1024 ||
        buf_init(&c->job.request, 0, req->head_len + req->body.len + 1024) != 0) {
        (void)plan_error(&c->plan, 500);
        return -1;
    }
    return 0;
}

static relay_t *start_proxy_attempt(conn_t *c, const http_request_t *req, const route_t *rt)
{
    const config_t *cfg = c->env->cfg;
    relay_t *relay =
        relay_start(&c->job, c->env->lim, platform_now_ms(), cfg->upstream_connect_timeout_ms,
                    cfg->upstream_response_timeout_ms);
    if (relay == NULL) {
        (void)plan_error(&c->plan, 500);
        return NULL;
    }
    if (c->job.backend == NULL) {
        relay_free(relay);
        (void)plan_error(&c->plan, c->job.attempts == 1 ? 503 : 502);
        return NULL;
    }
    if (client_gone(c, 0)) {
        relay_free(relay);
        c->client_canceled = true;
        return NULL;
    }
    if (relay_fd(relay) >= 0 && write_proxy_request(c, req, rt) != 0) {
        relay_free(relay);
        (void)plan_error(&c->plan, 500);
        return NULL;
    }
    return relay;
}

/* 현재 시도를 회수하고 다음 백엔드가 필요할 때만 true를 돌려준다. */
static bool finish_proxy_attempt(conn_t *c, relay_t *relay, relay_status_t result)
{
    upstream_conn_t *uc = NULL;
    http_response_t resp;
    proxy_fail_t why = PFAIL_BAD_RESPONSE;
    relay_take(relay, &uc, &resp, &why);
    relay_free(relay);
    if (c->client_canceled) {
        return false;
    }
    if (result == RELAY_OK) {
        if (c->job.client_minor == 0 && resp.framing.kind == FRAMING_CHUNKED) {
            upstream_release(uc, false);
            (void)plan_error(&c->plan, 502);
        } else {
            c->up_head_len = resp.head_len;
            c->up_reusable = resp.keep_alive;
            (void)proxy_on_head(&c->job, uc, &resp, &c->plan);
        }
        return false;
    }
    backend_t *failed = c->job.backend;
    if (proxy_on_fail(&c->job, why, &c->plan) == PROXY_GIVE_UP) {
        return false;
    }
    c->job.backend = failed;
    buf_reset(&c->job.request);
    return true;
}

/* 슬롯을 예약한 뒤 해당 백엔드의 Host로 직렬화한다. */
static void proxy_run(conn_t *c, const http_request_t *req, const route_t *rt)
{
    if (client_gone(c, 0)) {
        c->client_canceled = true;
        return;
    }
    if (setup_proxy_job(c, req, rt) != 0) {
        return;
    }
    for (;;) {
        relay_t *relay = start_proxy_attempt(c, req, rt);
        if (relay == NULL) {
            return;
        }
        relay_status_t result = drive_relay(c, relay, &c->client_canceled);
        if (!finish_proxy_attempt(c, relay, result)) {
            return;
        }
    }
}

/* 공용 추적기로 검증한 뒤 데이터만 정규 CRLF 청크로 다시 쓴다. */
static http_body_result_t relay_chunk(conn_t *c, http_body_tracker_t *tracker, chunk_relay_t *chunk,
                                      const char *data, size_t n, uint64_t *sent,
                                      bool *out_client_error)
{
    while (n > 0) {
        size_t piece;
        const bool payload = chunk->part == CHUNK_PAYLOAD;
        const char *lf = NULL;
        if (payload) {
            piece = tracker->remaining < n ? (size_t)tracker->remaining : n;
        } else {
            lf = memchr(data, '\n', n);
            piece = lf == NULL ? n : (size_t)(lf - data) + 1;
            if (piece > c->env->lim->max_head - chunk->metadata) {
                return BODY_ERROR;
            }
            chunk->metadata += piece;
        }

        size_t used;
        http_body_result_t result = http_body_track(tracker, data, piece, &used);
        if (result == BODY_ERROR || used != piece || (result == BODY_DONE && piece != n)) {
            return BODY_ERROR;
        }
        if (result == BODY_DONE) {
            if (response_write_last_chunk(&c->wbuf) != 0 ||
                (c->event == NULL && flush_wbuf(c) != 0)) {
                *out_client_error = true;
                return BODY_ERROR;
            }
            return BODY_DONE;
        }
        if (payload) {
            if (send_body_piece(c, data, piece, true) != 0) {
                *out_client_error = true;
                return BODY_ERROR;
            }
            *sent += piece;
            if (tracker->remaining == 0) {
                chunk->part = CHUNK_SEPARATOR;
            }
        } else if (lf != NULL && chunk->part != CHUNK_TRAILERS) {
            if (chunk->part == CHUNK_SIZE_LINE) {
                chunk->part = tracker->remaining == 0 ? CHUNK_TRAILERS : CHUNK_PAYLOAD;
            } else {
                chunk->part = CHUNK_SIZE_LINE;
            }
            chunk->metadata = 0;
        }
        data += piece;
        n -= piece;
    }
    return BODY_MORE;
}

/* 본문이 완전히 검증된 경우만 성공으로 보고한다. 클라이언트 취소는 건강 상태에 반영하지 않는다. */
static int upstream_send_body(conn_t *c, uint64_t *sent)
{
    response_plan_t *p = &c->plan;
    upstream_conn_t *uc = p->body.upstream.conn;
    http_body_tracker_t *t = &p->body.upstream.tracker;
    size_t buffered = uc->rbuf.len - c->up_head_len;
    const char *data = uc->rbuf.data + c->up_head_len;
    char chunk[CONN_IO_CHUNK];
    http_body_result_t result = BODY_MORE;
    bool canceled = false;
    chunk_relay_t chunk_relay = {0};
    bool upstream_failed = false;
    int64_t deadline = platform_now_ms() + c->env->cfg->upstream_response_timeout_ms;

    for (;;) {
        if (t->f.kind == FRAMING_CHUNKED) {
            result = relay_chunk(c, t, &chunk_relay, data, buffered, sent, &canceled);
        } else {
            size_t used = 0;
            result = http_body_track(t, data, buffered, &used);
            if (result == BODY_DONE && used != buffered) {
                result = BODY_ERROR;
            }
            if (result != BODY_ERROR && send_all(c, data, used) != 0) {
                canceled = true;
                result = BODY_ERROR;
            }
            *sent += used;
        }
        if (result == BODY_ERROR) {
            upstream_failed = !canceled;
            break;
        }
        if (result == BODY_DONE) {
            break;
        }
        if (buffered > 0) {
            deadline = platform_now_ms() + c->env->cfg->upstream_response_timeout_ms;
        }
        int ready = proxy_wait(c, uc->fd, POLLIN, deadline);
        if (ready <= 0) {
            canceled = ready == -2;
            upstream_failed = !canceled;
            break;
        }
        ssize_t n;
        do {
            n = read(uc->fd, chunk, sizeof(chunk));
        } while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            data = chunk;
            buffered = 0;
            continue;
        }
        if (n <= 0) {
            upstream_failed = !(n == 0 && t->f.kind == FRAMING_CLOSE);
            break;
        }
        data = chunk;
        buffered = (size_t)n;
    }
    if (!canceled) {
        upstream_report(uc->backend, !upstream_failed, platform_now_ms());
    }
    upstream_release(uc,
                     !canceled && !upstream_failed && c->up_reusable && t->f.kind != FRAMING_CLOSE);
    p->body.upstream.conn = NULL;
    return canceled || upstream_failed ? -1 : 0;
}

/* ── 요청 하나 ───────────────────────────────────────────────────────────── */

/* D-035: HTTP 머리 한도를 넘긴 바이너리 시작 줄은 응답 없이 닫는다. */
static bool binary_start_line(const buf_t *input)
{
    const char *lf = memchr(input->data, '\n', input->len);
    size_t len = lf != NULL ? (size_t)(lf - input->data) : input->len;

    return memchr(input->data, '\0', len) != NULL;
}

/* 요청 머리를 받을 때까지 읽는다. 1 DONE, 0 닫기(응답 없음 또는 에러 응답 완료). */
static int recv_head(conn_t *c, http_request_t *req)
{
    for (;;) {
        int st = 0;
        http_parse_result_t r =
            request_parse_head(c->rbuf.data, c->rbuf.len, c->env->lim, req, &st);
        int64_t dl;
        io_result_t io;

        if (r == HTTP_PARSE_DONE) {
            return 1;
        }
        if (r == HTTP_PARSE_ERROR) {
            if (st == 431 && binary_start_line(&c->rbuf)) {
                return 0;
            }
            return respond_error(c, st, 1);
        }
        dl = (c->rbuf.len == 0 && c->idle_deadline_ms >= 0) ? c->idle_deadline_ms
                                                            : c->head_deadline_ms;
        io = read_more(c, dl);
        if (io == IO_TIMEOUT) {
            /* 받은 바이트가 없으면(유휴) 응답 없이 닫는다 */
            return c->rbuf.len == 0 ? 0 : respond_error(c, 408, 1);
        }
        if (io != IO_OK) {
            return 0;
        }
    }
}

/* Content-Length 본문이 다 모일 때까지 읽는다. 1 완료, 0 닫기. */
static int recv_body(conn_t *c, http_request_t *req)
{
    size_t head_len = req->head_len;
    size_t need;
    int64_t dl;
    int st = 0;

    if (req->framing.kind != FRAMING_LENGTH || req->framing.length == 0) {
        return 1;
    }
    if (req->framing.length > c->rbuf.max - head_len) {
        return respond_error(c, 413, req->minor);
    }
    need = head_len + (size_t)req->framing.length;
    if (c->rbuf.len < need) {
        dl = platform_now_ms() + c->env->cfg->body_timeout_ms;
        while (c->rbuf.len < need) {
            io_result_t io = read_more(c, dl);

            if (io == IO_TIMEOUT) {
                return respond_error(c, 408, req->minor);
            }
            if (io != IO_OK) {
                return 0;
            }
        }
        /* 읽는 동안 rbuf가 재할당됐을 수 있다. 같은 머리를 다시 파싱해 str_t를 새로 잡는다 */
        int minor = req->minor;
        if (request_parse_head(c->rbuf.data, head_len, c->env->lim, req, &st) != HTTP_PARSE_DONE) {
            return respond_error(c, 500, minor);
        }
    }
    req->body.ptr = c->rbuf.data + req->head_len;
    req->body.len = (size_t)req->framing.length;
    return 1;
}

/* 숫자 루프백 주소와 단일 업스트림의 포트를 연결을 열기 전에 대조한다. */
static int forward_allowed(const conn_t *c, const http_request_t *req, const route_t *rt)
{
    const str_t authority = req->host;
    static const char loopback[] = "127.0.0.1";
    size_t host_len = sizeof(loopback) - 1;
    if (c->forward_port == 0 || strcmp(c->ip, loopback) != 0) {
        return 403;
    }
    if (req->method != HTTP_GET && req->method != HTTP_POST) {
        return 405;
    }
    if (authority.len < host_len || memcmp(authority.ptr, loopback, host_len) != 0 ||
        (authority.len > host_len && authority.ptr[host_len] != ':')) {
        return 403;
    }
    uint64_t port = 80;
    if (authority.len > host_len) {
        str_t port_text = {authority.ptr + host_len + 1, authority.len - host_len - 1};
        if (str_to_u64(port_text, 65535, &port) != 0 || port == 0) {
            return 400;
        }
    }
    if (port != c->forward_port || port == c->server_port || rt == NULL ||
        rt->kind != ROUTE_PROXY || rt->host == NULL || strcmp(rt->host, loopback) != 0 ||
        upstream_backend_count(rt->upstream) != 1) {
        return 403;
    }
    backend_t *b = upstream_pick(rt->upstream, NULL, platform_now_ms());
    if (b == NULL) {
        return 503;
    }
    char endpoint[sizeof(loopback) + 6];
    (void)snprintf(endpoint, sizeof(endpoint), "%s:%u", loopback, (unsigned)port);
    return strcmp(backend_hostport(b), endpoint) == 0 ? 0 : 403;
}

static const route_t *resolve_route(conn_t *c, const http_request_t *req, route_t *forward)
{
    const route_t *rt = router_match(c->env->router, req->host, req->path);
    if (req->target.ptr[0] != '/') {
        int status = forward_allowed(c, req, rt);
        if (status != 0) {
            (void)plan_error(&c->plan, status);
            if (status == 405) {
                (void)plan_add_header(&c->plan, "Allow", "GET, POST");
            }
            return NULL;
        }
        *forward = *rt;
        forward->keep_host = true;
        forward->strip_prefix = false;
        return forward;
    }
    if (rt == NULL) {
        (void)plan_error(&c->plan, 404);
    }
    return rt;
}

static void dispatch_local(conn_t *c, const http_request_t *req, const route_t *rt, int64_t now)
{
    handler_ctx_t ctx = {
        .cfg = c->env->cfg,
        .lim = c->env->lim,
        .client_ip = str_from_cstr(c->ip),
        .server_port = c->server_port,
        .now = platform_wall_time(),
        .now_ms = now,
    };
    (void)handler_dispatch(req, rt, &ctx, &c->job, &c->plan);
}

/* 계획을 만든다: 라우팅 → 처리기 → (프록시 루프) → (CGI 머리). */
static void build_plan(conn_t *c, const http_request_t *req, size_t *out_cgi_body_off)
{
    route_t forward;
    const route_t *rt = resolve_route(c, req, &forward);
    *out_cgi_body_off = 0;
    if (rt == NULL) {
        return;
    }
    if (rt->kind == ROUTE_PROXY) {
        proxy_run(c, req, rt);
    } else {
        dispatch_local(c, req, rt, platform_now_ms());
    }
    if (c->plan.kind == BODY_PIPE && c->plan.head_pending) {
        cgi_resolve_head(c, req->minor, out_cgi_body_off);
    }
}

static int send_response(conn_t *c, const http_request_t *req, size_t cgi_body_off, uint64_t *sent)
{
    response_plan_t *p = &c->plan;
    bool with_body = req->method != HTTP_HEAD && !response_status_has_no_body(p->head.status);

    if (send_head_and_mem(c, req->minor, with_body, sent) != 0) {
        return -1;
    }
    switch (p->kind) {
    case BODY_FILE:
        return with_body ? send_file(c, p->body.file.fd, p->body.file.off, p->body.file.len, sent)
                         : 0;
    case BODY_PIPE:
        return with_body ? cgi_send_body(c, c->cgi_out.data + cgi_body_off,
                                         c->cgi_out.len - cgi_body_off, sent)
                         : 0;
    case BODY_UPSTREAM:
        if (!with_body && p->body.upstream.tracker.f.kind != FRAMING_NONE) {
            return 0;
        }
        return upstream_send_body(c, sent);
    case BODY_NONE:
    case BODY_MEM:
    default:
        return 0;
    }
}

/* 요청 하나를 처리한다. 연결을 유지하면 true. */
static bool serve_one(conn_t *c)
{
    const config_t *cfg = c->env->cfg;
    http_request_t req;
    char method[16];
    char target[CONN_LOG_TARGET_MAX];
    size_t cgi_body_off = 0;
    uint64_t sent = 0;
    int64_t start;
    bool close_after;
    http_framing_kind_t fk;
    int rc;

    if (recv_head(c, &req) == 0) {
        return false;
    }
    start = platform_now_ms();
    if (recv_body(c, &req) == 0) {
        return false;
    }
    c->n_requests++;
    close_after = !req.keep_alive || c->n_requests >= cfg->max_keepalive_requests;

    plan_init(&c->plan);
    memset(&c->job, 0, sizeof(c->job));
    build_plan(c, &req, &cgi_body_off);
    if (close_after) {
        c->plan.head.close = true; /* 그 응답에 Connection: close */
    }
    rc = c->client_canceled ? -1 : send_response(c, &req, cgi_body_off, &sent);
    fk = c->plan.head.framing.kind;
    /* response_write_head가 Connection: close를 넣는 경우와 같게 닫는다 */
    close_after = close_after || rc != 0 || c->plan.head.close || fk == FRAMING_CLOSE ||
                  (fk == FRAMING_CHUNKED && req.minor == 0);

    if (!c->client_canceled) {
        copy_cstr(req.method_raw, method, sizeof(method));
        copy_cstr(req.target, target, sizeof(target));
        log_access(c->ip, method, target, c->plan.head.status, sent, platform_now_ms() - start);
    }
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    buf_cleanup(&c->cgi_out);
    if (close_after) {
        return false;
    }

    /* 처리한 요청만 지운다. 파이프라이닝으로 남은 바이트는 다음 요청이다 */
    buf_consume(&c->rbuf, req.head_len + req.body.len);
    c->head_deadline_ms = platform_now_ms() + cfg->header_timeout_ms;
    c->idle_deadline_ms = platform_now_ms() + cfg->keepalive_timeout_ms;
    return true;
}

/* ── v2 이벤트 경로 ─────────────────────────────────────────────────────── */

static bool again(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void nb_unwatch(conn_t *c, int *fd)
{
    if (*fd >= 0) {
        (void)engine_unwatch(c->env->engine, *fd);
        *fd = -1;
    }
}

static void nb_unwatch_aux(conn_t *c)
{
    nb_unwatch(c, &c->event->source_fd);
    nb_unwatch(c, &c->event->cgi_in_fd);
}

static conn_status_t nb_wait(conn_t *c, conn_status_t status)
{
    c->event->waiting = status;
    return status;
}

static conn_status_t nb_wait_source(conn_t *c, int fd, unsigned mask)
{
    conn_event_t *s = c->event;
    if (s->source_fd != fd) {
        nb_unwatch(c, &s->source_fd);
    }
    if (s->source_fd != fd || s->source_events != mask) {
        if (engine_watch(c->env->engine, fd, mask, c) != 0) {
            return nb_wait(c, CONN_CLOSE);
        }
        s->source_fd = fd;
        s->source_events = mask;
    }
    if (engine_watch(c->env->engine, c->fd, s->pending_input ? 0 : EV_READ, c) != 0) {
        return nb_wait(c, CONN_CLOSE);
    }
    return nb_wait(c, CONN_WAIT_FD);
}

static conn_status_t nb_wait_write(conn_t *c)
{
    /* 클라이언트가 느릴 때 응답 소스는 더 읽지 않고 CGI stdin만 계속 밀어 넣는다. */
    nb_unwatch(c, &c->event->source_fd);
    return nb_wait(c, CONN_WANT_WRITE);
}

static void nb_prepare_response(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    response_plan_t *p = &c->plan;
    p->head.close |= s->close_after;
    s->close_after |= p->head.close || p->head.framing.kind == FRAMING_CLOSE;
    s->with_body = s->req.method != HTTP_HEAD && !response_status_has_no_body(p->head.status);
    buf_reset(&c->wbuf);
    s->write_off = 0;
    s->phase = NB_SEND;
    s->deadline = now + c->env->cfg->body_timeout_ms;
    if (response_write_head(&p->head, s->minor, platform_wall_time(), &c->wbuf) != 0) {
        s->phase = NB_CLOSE;
        return;
    }
    s->log_response = true;
    if (p->kind == BODY_MEM && s->with_body) {
        if (buf_append(&c->wbuf, p->body.mem.data.data, p->body.mem.data.len) != 0) {
            s->phase = NB_CLOSE;
        } else {
            s->sent = p->body.mem.data.len;
        }
    }
}

static void nb_error(conn_t *c, int status, bool request_error, int64_t now)
{
    conn_event_t *s = c->event;
    nb_unwatch_aux(c);
    relay_free(s->relay);
    s->relay = NULL;
    if (request_error) {
        s->parser_error = true;
        s->close_after = true;
    }
    (void)plan_error(&c->plan, status);
    nb_prepare_response(c, now);
}

static void nb_cgi_start(conn_t *c, int64_t now)
{
    response_plan_t *p = &c->plan;
    const config_t *cfg = c->env->cfg;
    conn_event_t *s = c->event;
    int in = p->body.pipe.in_fd;
    if (buf_init(&c->cgi_out, 4096, cfg->cgi_max_head + CONN_IO_CHUNK) != 0 ||
        platform_set_nonblock(p->body.pipe.out_fd) != 0 ||
        (in >= 0 && platform_set_nonblock(in) != 0)) {
        nb_error(c, 500, false, now);
        return;
    }
    if (in >= 0 && p->body.pipe.in_data.len == 0) {
        close(in);
        p->body.pipe.in_fd = -1;
    } else if (in >= 0) {
        if (engine_watch(c->env->engine, in, EV_WRITE, c) != 0) {
            nb_error(c, 500, false, now);
            return;
        }
        s->cgi_in_fd = in;
    }
    s->phase = NB_CGI_HEAD;
    s->deadline = p->body.pipe.deadline_ms;
}

static void nb_cgi_stdin(conn_t *c)
{
    conn_event_t *s = c->event;
    buf_t *data = &c->plan.body.pipe.in_data;
    size_t left = data->len - s->cgi_in_off;
    ssize_t n = write(s->cgi_in_fd, data->data + s->cgi_in_off,
                      left < CONN_IO_CHUNK ? left : CONN_IO_CHUNK);
    if (n > 0) {
        s->cgi_in_off += (size_t)n;
        if (s->cgi_in_off != data->len) {
            return;
        }
    } else if (n < 0 && again()) {
        return;
    }
    int fd = s->cgi_in_fd;
    nb_unwatch(c, &s->cgi_in_fd);
    close(fd);
    c->plan.body.pipe.in_fd = -1;
}

static void nb_dispatch_request(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    route_t forward;
    const route_t *rt = resolve_route(c, &s->req, &forward);
    c->n_requests++;
    s->close_after = !s->req.keep_alive || c->n_requests >= c->env->cfg->max_keepalive_requests;
    if (rt == NULL) {
        nb_prepare_response(c, now);
        return;
    }
    if (rt->kind != ROUTE_STATIC && c->env->engine == NULL) {
        nb_error(c, 500, false, now); /* D-025 */
        return;
    }
    if (rt->kind == ROUTE_PROXY) {
        s->route = *rt;
        if (setup_proxy_job(c, &s->req, &s->route) == 0) {
            s->phase = NB_RELAY_HEAD;
            return;
        }
    } else {
        dispatch_local(c, &s->req, rt, now);
        if (c->plan.kind == BODY_PIPE) {
            nb_cgi_start(c, now);
            return;
        }
    }
    nb_prepare_response(c, now);
}

static void nb_log_response(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    if (!s->log_response) {
        return;
    }
    s->log_response = false;
    char method[16];
    char target[CONN_LOG_TARGET_MAX];
    if (s->parser_error) {
        log_access(c->ip, "-", "-", c->plan.head.status, s->flushed, 0);
    } else {
        copy_cstr(s->req.method_raw, method, sizeof(method));
        copy_cstr(s->req.target, target, sizeof(target));
        log_access(c->ip, method, target, c->plan.head.status, s->flushed, now - s->started);
    }
}

static void nb_finish_response(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    nb_log_response(c, now);
    nb_unwatch_aux(c);
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    buf_cleanup(&c->cgi_out);
    if (s->close_after) {
        if (s->parser_error) {
            shutdown(c->fd, SHUT_WR);
            s->phase = NB_DRAIN;
            s->deadline = now + CONN_DRAIN_MS;
        } else {
            s->phase = NB_CLOSE;
        }
        return;
    }
    buf_consume(&c->rbuf, s->req.head_len + s->req.body.len);
    c->head_deadline_ms = now + c->env->cfg->header_timeout_ms;
    c->idle_deadline_ms = now + c->env->cfg->keepalive_timeout_ms;
    *s = (conn_event_t){
        .source_fd = -1,
        .cgi_in_fd = -1,
        .deadline = c->rbuf.len == 0 ? c->idle_deadline_ms : c->head_deadline_ms,
    };
}

/* read / write가 다시 필요하면 해당 interest를 돌려주고, 다른 단계 진입은 0으로 알린다. */
typedef enum { NB_PROGRESS, NB_NEED_CLIENT_READ, NB_NEED_CLIENT_WRITE, NB_NEED_SOURCE } nb_step_t;

static nb_step_t nb_input(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    http_request_t *req = &s->req;
    if (s->phase == NB_HEAD) {
        int status = 0;
        http_parse_result_t parsed =
            request_parse_head(c->rbuf.data, c->rbuf.len, c->env->lim, req, &status);
        if (parsed == HTTP_PARSE_ERROR) {
            if (status == 431 && binary_start_line(&c->rbuf)) {
                s->phase = NB_CLOSE;
            } else {
                s->minor = 1;
                nb_error(c, status, true, now);
            }
            return NB_PROGRESS;
        }
        if (parsed == HTTP_PARSE_DONE) {
            s->started = now;
            s->minor = req->minor;
            if (req->framing.kind != FRAMING_LENGTH || req->framing.length == 0) {
                nb_dispatch_request(c, now);
            } else {
                s->phase = NB_REQUEST_BODY;
                s->deadline = now + c->env->cfg->body_timeout_ms;
            }
            return NB_PROGRESS;
        }
    } else {
        size_t head_len = req->head_len;
        uint64_t length = req->framing.length;
        if (length > c->rbuf.max - head_len) {
            nb_error(c, 413, true, now);
            return NB_PROGRESS;
        }
        if (c->rbuf.len >= head_len + (size_t)length) {
            int ignored;
            if (request_parse_head(c->rbuf.data, head_len, c->env->lim, req, &ignored) !=
                HTTP_PARSE_DONE) {
                nb_error(c, 500, true, now);
            } else {
                req->body = (str_t){c->rbuf.data + head_len, (size_t)length};
                nb_dispatch_request(c, now);
            }
            return NB_PROGRESS;
        }
    }
    ssize_t n = buf_read_fd(&c->rbuf, c->fd);
    if (n < 0 && again()) {
        return NB_NEED_CLIENT_READ;
    }
    if (n <= 0) {
        s->phase = NB_CLOSE;
    } else if (s->phase == NB_HEAD) {
        s->deadline = c->head_deadline_ms;
    }
    return NB_PROGRESS;
}

static nb_step_t nb_relay_head(conn_t *c, unsigned events, int64_t now)
{
    conn_event_t *s = c->event;
    if (s->relay == NULL) {
        s->relay = start_proxy_attempt(c, &s->req, &s->route);
        if (s->relay == NULL) {
            if (c->client_canceled) {
                s->phase = NB_CLOSE;
            } else {
                nb_prepare_response(c, now);
            }
            return NB_PROGRESS;
        }
        events = 0;
    }
    /* relay_step은 기존 fd를 닫거나 같은 번호를 재사용할 수 있다. */
    nb_unwatch(c, &s->source_fd);
    relay_status_t status = relay_step(s->relay, events, now);
    if (status == RELAY_WANT_READ || status == RELAY_WANT_WRITE) {
        s->deadline = relay_deadline_ms(s->relay);
        s->source_events = status == RELAY_WANT_READ ? EV_READ : EV_WRITE;
        return NB_NEED_SOURCE;
    }
    bool retry = finish_proxy_attempt(c, s->relay, status);
    s->relay = NULL;
    if (!retry) {
        nb_prepare_response(c, now);
    }
    return NB_PROGRESS;
}

static nb_step_t nb_cgi_head(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    http_head_out_t h = {0};
    size_t used = 0;
    int ignored;
    http_parse_result_t parsed = cgi_parse_head(c->cgi_out.data, c->cgi_out.len,
                                                c->env->cfg->cgi_max_head, &h, &used, &ignored);
    if (parsed == HTTP_PARSE_DONE) {
        take_cgi_head(&c->plan, &h, s->minor);
        s->body_off = used;
        nb_prepare_response(c, now);
        return NB_PROGRESS;
    }
    buf_cleanup(&h.fields);
    if (parsed == HTTP_PARSE_ERROR) {
        nb_error(c, 502, false, now);
        return NB_PROGRESS;
    }
    ssize_t n = buf_read_fd(&c->cgi_out, c->plan.body.pipe.out_fd);
    if (n < 0 && again()) {
        return NB_NEED_SOURCE;
    }
    if (n <= 0) {
        nb_error(c, 502, false, now);
    }
    return NB_PROGRESS;
}

static void nb_upstream_finish(conn_t *c, bool ok, int64_t now)
{
    response_plan_t *p = &c->plan;
    upstream_conn_t *uc = p->body.upstream.conn;
    nb_unwatch(c, &c->event->source_fd);
    if (uc != NULL) {
        upstream_report(uc->backend, ok, now);
        upstream_release(uc,
                         ok && c->up_reusable && p->body.upstream.tracker.f.kind != FRAMING_CLOSE);
        p->body.upstream.conn = NULL;
    }
}

static nb_step_t nb_upstream_body(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    response_plan_t *p = &c->plan;
    upstream_conn_t *uc = p->body.upstream.conn;
    http_body_tracker_t *t = &p->body.upstream.tracker;
    if (s->body_done) {
        nb_upstream_finish(c, true, now);
        nb_finish_response(c, now);
        return NB_PROGRESS;
    }
    char tmp[CONN_IO_CHUNK];
    const char *bytes;
    ssize_t n;
    if (c->up_head_len != 0) {
        bytes = uc->rbuf.data + c->up_head_len;
        n = (ssize_t)(uc->rbuf.len - c->up_head_len);
        c->up_head_len = 0;
    } else {
        bytes = tmp;
        n = read(uc->fd, tmp, sizeof(tmp));
        if (n < 0 && again()) {
            return NB_NEED_SOURCE;
        }
        if (n <= 0) {
            bool ok = n == 0 && t->f.kind == FRAMING_CLOSE;
            nb_upstream_finish(c, ok, now);
            if (ok) {
                nb_finish_response(c, now);
            } else {
                s->phase = NB_CLOSE;
            }
            return NB_PROGRESS;
        }
    }
    http_body_result_t result;
    bool local_error = false;
    if (t->f.kind == FRAMING_CHUNKED) {
        result = relay_chunk(c, t, &s->chunk, bytes, (size_t)n, &s->sent, &local_error);
    } else {
        size_t used = 0;
        result = http_body_track(t, bytes, (size_t)n, &used);
        if (used != (size_t)n) {
            result = BODY_ERROR;
        } else if (send_body_piece(c, bytes, used, false) != 0) {
            local_error = true;
            result = BODY_ERROR;
        }
        s->sent += used;
    }
    if (result == BODY_ERROR) {
        if (!local_error) {
            nb_upstream_finish(c, false, now);
            /* 같은 read에 정상 청크와 오류가 섞여도 이미 검증한 조각은 전송한다. */
            if (c->wbuf.len > 0) {
                s->stream_failed = true;
                return NB_PROGRESS;
            }
        }
        s->phase = NB_CLOSE;
        return NB_PROGRESS;
    }
    s->body_done = result == BODY_DONE;
    return NB_PROGRESS;
}

static nb_step_t nb_cgi_body(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    response_plan_t *p = &c->plan;
    http_framing_t f = p->head.framing;
    if (s->body_done || !s->with_body || (f.kind == FRAMING_LENGTH && s->sent == f.length)) {
        nb_finish_response(c, now);
        return NB_PROGRESS;
    }
    char tmp[CONN_IO_CHUNK];
    const char *bytes;
    ssize_t n;
    if (s->body_off < c->cgi_out.len) {
        bytes = c->cgi_out.data + s->body_off;
        n = (ssize_t)(c->cgi_out.len - s->body_off);
        s->body_off = c->cgi_out.len;
    } else {
        bytes = tmp;
        n = read(p->body.pipe.out_fd, tmp, sizeof(tmp));
        if (n < 0 && again()) {
            return NB_NEED_SOURCE;
        }
    }
    if (n < 0 || (n == 0 && f.kind == FRAMING_LENGTH)) {
        s->phase = NB_CLOSE;
    } else if (n == 0) {
        s->body_done = true;
        if (f.kind == FRAMING_CHUNKED && response_write_last_chunk(&c->wbuf) != 0) {
            s->phase = NB_CLOSE;
        }
    } else {
        size_t count = (size_t)n;
        if (f.kind == FRAMING_LENGTH && f.length - s->sent < count) {
            count = (size_t)(f.length - s->sent);
        }
        if (send_body_piece(c, bytes, count, f.kind == FRAMING_CHUNKED) != 0) {
            s->phase = NB_CLOSE;
        }
        s->sent += count;
    }
    return NB_PROGRESS;
}

static nb_step_t nb_send(conn_t *c, int64_t now)
{
    conn_event_t *s = c->event;
    response_plan_t *p = &c->plan;
    if (s->write_off < c->wbuf.len) {
        ssize_t n = platform_send(c->fd, c->wbuf.data + s->write_off, c->wbuf.len - s->write_off);
        if (n < 0 && again()) {
            return NB_NEED_CLIENT_WRITE;
        }
        if (n <= 0) {
            s->phase = NB_CLOSE;
            return NB_PROGRESS;
        }
        s->write_off += (size_t)n;
        s->deadline = now + c->env->cfg->body_timeout_ms;
        if (s->write_off == c->wbuf.len) {
            s->flushed = s->sent;
            s->write_off = 0;
            buf_reset(&c->wbuf);
        }
        return NB_PROGRESS;
    }
    if (s->stream_failed) {
        s->phase = NB_CLOSE;
        return NB_PROGRESS;
    }
    if (p->kind == BODY_UPSTREAM) {
        return nb_upstream_body(c, now);
    }
    if (p->kind == BODY_PIPE) {
        return nb_cgi_body(c, now);
    }
    if (p->kind == BODY_FILE && s->with_body && p->body.file.len > 0) {
        off_t *offset = &p->body.file.off;
        off_t before = *offset;
        size_t count = p->body.file.len < CONN_IO_CHUNK ? (size_t)p->body.file.len : CONN_IO_CHUNK;
        ssize_t n = platform_sendfile(c->fd, p->body.file.fd, offset, count);
        off_t sent = *offset - before;
        s->sent += (uint64_t)sent;
        s->flushed = s->sent;
        p->body.file.len -= sent;
        if (sent > 0) {
            s->deadline = now + c->env->cfg->body_timeout_ms;
        }
        if (n < 0 && again()) {
            return NB_NEED_CLIENT_WRITE;
        }
        if (n <= 0 && sent == 0) {
            s->phase = NB_CLOSE;
        }
        return NB_PROGRESS;
    }
    nb_finish_response(c, now);
    return NB_PROGRESS;
}

static conn_status_t nb_drive(conn_t *c, unsigned source_events, int64_t now)
{
    conn_event_t *s = c->event;
    for (int i = 0; i < CONN_EVENT_STEPS; i++) {
        nb_step_t next;
        switch (s->phase) {
        case NB_HEAD:
        case NB_REQUEST_BODY:
            next = nb_input(c, now);
            break;
        case NB_RELAY_HEAD:
            next = nb_relay_head(c, source_events, now);
            source_events = 0;
            break;
        case NB_CGI_HEAD:
            next = nb_cgi_head(c, now);
            break;
        case NB_SEND:
            if (c->plan.kind == BODY_PIPE && now >= c->plan.body.pipe.deadline_ms) {
                return nb_wait(c, CONN_CLOSE);
            }
            next = nb_send(c, now);
            break;
        case NB_DRAIN: {
            char buffer[CONN_IO_CHUNK];
            ssize_t n = read(c->fd, buffer, sizeof(buffer));
            if (n < 0 && again()) {
                return nb_wait(c, CONN_WANT_READ);
            }
            if (n <= 0) {
                return nb_wait(c, CONN_CLOSE);
            }
            continue;
        }
        case NB_CLOSE:
        default:
            return nb_wait(c, CONN_CLOSE);
        }
        if (next == NB_NEED_CLIENT_READ) {
            return nb_wait(c, CONN_WANT_READ);
        }
        if (next == NB_NEED_CLIENT_WRITE) {
            return nb_wait_write(c);
        }
        if (next == NB_NEED_SOURCE) {
            if (s->phase == NB_RELAY_HEAD) {
                return nb_wait_source(c, relay_fd(s->relay), s->source_events);
            }
            if (c->plan.kind == BODY_PIPE) {
                s->deadline = c->plan.body.pipe.deadline_ms;
                return nb_wait_source(c, c->plan.body.pipe.out_fd, EV_READ);
            }
            s->deadline = now + c->env->cfg->upstream_response_timeout_ms;
            return nb_wait_source(c, c->plan.body.upstream.conn->fd, EV_READ);
        }
    }
    /* 쓰기 가능한 클라이언트로 이어서 실행하고 다른 연결의 이벤트도 처리한다. */
    return s->phase == NB_DRAIN ? nb_wait(c, CONN_WANT_READ) : nb_wait_write(c);
}

/* ── 공개 함수 ───────────────────────────────────────────────────────────── */

conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env, int64_t now_ms)
{
    conn_t *c;
    int e;

    if (env->lim->max_body > SIZE_MAX - env->lim->max_head) {
        errno = EOVERFLOW;
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    c->fd = -1; /* 실패 시 conn_free가 호출자의 fd를 닫지 않게 */
    c->env = env;
    const char *forward = getenv("HTTPD_FORWARD_PROXY");
    uint64_t forward_port;
    if (forward != NULL && strcmp(env->cfg->listen_host, "127.0.0.1") == 0 &&
        str_to_u64(str_from_cstr(forward), 65535, &forward_port) == 0) {
        c->forward_port = (uint16_t)forward_port;
    }
    if (env->nonblocking) {
        c->event = calloc(1, sizeof(*c->event));
        if (c->event == NULL) {
            conn_free(c);
            return NULL;
        }
        c->event->source_fd = -1;
        c->event->cgi_in_fd = -1;
        c->event->deadline = now_ms + env->cfg->header_timeout_ms;
    }
    plan_init(&c->plan);
    if (set_mode(fd, env->nonblocking) != 0 ||
        buf_init(&c->rbuf, 4096, env->lim->max_head + (size_t)env->lim->max_body) != 0 ||
        buf_init(&c->wbuf, 4096, CONN_WBUF_MAX) != 0) {
        e = errno;
        conn_free(c);
        errno = e;
        return NULL;
    }
    c->fd = fd;
    format_ip(peer, c->ip, sizeof(c->ip));
    c->server_port = local_port(fd);
    c->head_deadline_ms = now_ms + env->cfg->header_timeout_ms;
    c->idle_deadline_ms = -1;
    return c;
}

void conn_free(conn_t *c)
{
    int e = errno;

    if (c == NULL) {
        return;
    }
    if (c->event != NULL) {
        nb_log_response(c, platform_now_ms());
        nb_unwatch_aux(c);
        relay_free(c->event->relay);
        free(c->event);
    }
    plan_cleanup(&c->plan);
    proxy_job_cleanup(&c->job);
    buf_cleanup(&c->rbuf);
    buf_cleanup(&c->wbuf);
    buf_cleanup(&c->cgi_out);
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
    errno = e;
}

void conn_serve_blocking(conn_t *c)
{
    if (c->env->nonblocking) {
        return;
    }
    while (serve_one(c)) {
    }
}

conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms)
{
    conn_event_t *s = c->event;
    if (s == NULL) {
        return CONN_CLOSE;
    }
    int64_t deadline = conn_deadline_ms(c);
    if (deadline >= 0 && now_ms >= deadline) {
        return conn_on_timeout(c, now_ms);
    }
    if (fd != c->fd) {
        if (fd == s->cgi_in_fd) {
            nb_cgi_stdin(c);
            return s->waiting;
        }
        if (fd == s->source_fd && s->waiting == CONN_WAIT_FD) {
            return nb_drive(c, events, now_ms);
        }
        return s->waiting;
    }
    if (s->waiting == CONN_WAIT_FD) {
        if ((events & EV_ERROR) || client_gone(c, 0)) {
            return nb_wait(c, CONN_CLOSE);
        }
        s->pending_input = true;
        if (engine_watch(c->env->engine, c->fd, 0, c) != 0) {
            return nb_wait(c, CONN_CLOSE);
        }
        return CONN_WAIT_FD;
    }
    return nb_drive(c, 0, now_ms);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    conn_event_t *s = c->event;
    if (s == NULL) {
        return CONN_CLOSE;
    }
    int64_t deadline = conn_deadline_ms(c);
    if (deadline < 0 || now_ms < deadline) {
        return s->waiting;
    }
    switch (s->phase) {
    case NB_HEAD:
        if (c->rbuf.len == 0) {
            return nb_wait(c, CONN_CLOSE);
        }
        s->minor = 1;
        nb_error(c, 408, true, now_ms);
        break;
    case NB_REQUEST_BODY:
        nb_error(c, 408, true, now_ms);
        break;
    case NB_RELAY_HEAD:
        return nb_drive(c, 0, now_ms); /* relay가 마감 종류에 맞는 결과를 만든다. */
    case NB_CGI_HEAD:
        nb_error(c, 504, false, now_ms);
        break;
    case NB_SEND:
        if (s->waiting == CONN_WAIT_FD && c->plan.kind == BODY_UPSTREAM) {
            nb_upstream_finish(c, false, now_ms);
        }
        return nb_wait(c, CONN_CLOSE);
    default:
        return nb_wait(c, CONN_CLOSE);
    }
    return nb_drive(c, 0, now_ms);
}

int conn_fd(const conn_t *c)
{
    return c->fd;
}

int64_t conn_deadline_ms(const conn_t *c)
{
    if (c->event == NULL) {
        return -1;
    }
    int64_t deadline = c->event->deadline;
    if (c->plan.kind == BODY_PIPE && c->plan.body.pipe.deadline_ms >= 0) {
        deadline = min64(deadline, c->plan.body.pipe.deadline_ms);
    }
    return deadline;
}
