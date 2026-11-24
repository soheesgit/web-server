/* src/downstream/conn.c
 *
 * 연결 상태 머신. 명세: conn.h
 * v1(blocking) 경로만 구현했다. v2(conn_on_event·conn_on_timeout·conn_deadline_ms)는 7단계.
 */
#include "downstream/conn.h"

#include "common/http_framing.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/stub.h"
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

struct conn {
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
        return send_all(c, p, n);
    }
    if (response_write_chunk(p, n, &c->wbuf) != 0) {
        return -1;
    }
    return flush_wbuf(c);
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
            /* D-029: Content-Length가 없으면 1.1은 chunked, 1.0은 연결 종료로 끝낸다 */
            if (response_status_has_no_body(h.status)) {
                h.framing = (http_framing_t){FRAMING_NONE, 0};
            } else if (h.framing.kind == FRAMING_NONE) {
                h.framing.kind = minor >= 1 ? FRAMING_CHUNKED : FRAMING_CLOSE;
            }
            h.close = p->head.close;
            buf_cleanup(&p->head.fields);
            p->head = h;
            p->head_pending = false;
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

/* 슬롯을 예약한 뒤 해당 백엔드의 Host로 쓰고, 실패한 시도는 반납한 뒤 다음을 예약한다. */
static void proxy_run(conn_t *c, const http_request_t *req, const route_t *rt)
{
    const config_t *cfg = c->env->cfg;
    proxy_job_t *job = &c->job;

    if (client_gone(c, 0)) {
        c->client_canceled = true;
        return;
    }
    *job = (proxy_job_t){
        .group = rt->upstream,
        .method = req->method,
        .attempts = 1,
        .max_attempts = (int)upstream_backend_count(rt->upstream),
        .client_minor = req->minor,
        .client_keep_alive = req->keep_alive,
    };
    if (req->body.len > SIZE_MAX - req->head_len - 1024 ||
        buf_init(&job->request, 0, req->head_len + req->body.len + 1024) != 0) {
        (void)plan_error(&c->plan, 500);
        return;
    }
    for (;;) {
        upstream_conn_t *uc = NULL;
        http_response_t resp;
        proxy_fail_t why = PFAIL_BAD_RESPONSE;
        relay_t *relay =
            relay_start(job, c->env->lim, platform_now_ms(), cfg->upstream_connect_timeout_ms,
                        cfg->upstream_response_timeout_ms);
        if (relay == NULL) {
            (void)plan_error(&c->plan, 500);
            return;
        }
        if (job->backend == NULL) {
            relay_free(relay);
            (void)plan_error(&c->plan, job->attempts == 1 ? 503 : 502);
            return;
        }
        if (client_gone(c, 0)) {
            relay_free(relay);
            c->client_canceled = true;
            return;
        }
        if (relay_fd(relay) >= 0 && write_proxy_request(c, req, rt) != 0) {
            relay_free(relay);
            (void)plan_error(&c->plan, 500);
            return;
        }
        relay_status_t result = drive_relay(c, relay, &c->client_canceled);
        relay_take(relay, &uc, &resp, &why);
        relay_free(relay);
        if (c->client_canceled) {
            return;
        }
        if (result == RELAY_OK) {
            if (req->minor == 0 && resp.framing.kind == FRAMING_CHUNKED) {
                upstream_release(uc, false);
                (void)plan_error(&c->plan, 502);
                return;
            }
            c->up_head_len = resp.head_len;
            c->up_reusable = resp.keep_alive;
            (void)proxy_on_head(job, uc, &resp, &c->plan);
            return;
        }
        backend_t *failed = job->backend;
        if (proxy_on_fail(job, why, &c->plan) == PROXY_GIVE_UP) {
            return;
        }
        job->backend = failed;
        buf_reset(&job->request);
    }
}

typedef enum { CHUNK_SIZE_LINE, CHUNK_PAYLOAD, CHUNK_SEPARATOR, CHUNK_TRAILERS } chunk_part_t;

typedef struct {
    chunk_part_t part;
    size_t metadata;
} chunk_relay_t;

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
            if (response_write_last_chunk(&c->wbuf) != 0 || flush_wbuf(c) != 0) {
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

/* 계획을 만든다: 라우팅 → 처리기 → (프록시 루프) → (CGI 머리). */
static void build_plan(conn_t *c, const http_request_t *req, size_t *out_cgi_body_off)
{
    const route_t *rt = router_match(c->env->router, req->host, req->path);
    handler_ctx_t ctx;

    *out_cgi_body_off = 0;
    if (rt == NULL) {
        plan_error(&c->plan, 404);
        return;
    }
    ctx.cfg = c->env->cfg;
    ctx.lim = c->env->lim;
    ctx.client_ip = str_from_cstr(c->ip);
    ctx.server_port = c->server_port;
    ctx.now = platform_wall_time();
    ctx.now_ms = platform_now_ms();
    if (rt->kind == ROUTE_PROXY) {
        proxy_run(c, req, rt);
    } else {
        (void)handler_dispatch(req, rt, &ctx, &c->job, &c->plan);
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
    UNUSED(c);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(now_ms);
    /* TODO(downstream): 구현 — U-DS-40, U-DS-44 */
    STUB_RETURN(CONN_CLOSE);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(now_ms);
    /* TODO(downstream): 구현 — E-ER-10, E-ER-11, E-KA-06, E-SL-01, E-CG-07, E-PX-10 */
    STUB_RETURN(CONN_CLOSE);
}

int conn_fd(const conn_t *c)
{
    return c->fd;
}

int64_t conn_deadline_ms(const conn_t *c)
{
    UNUSED(c);
    /* TODO(downstream): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
