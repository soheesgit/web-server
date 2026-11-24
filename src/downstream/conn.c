/* src/downstream/conn.c
 *
 * 연결 상태 머신. 명세: conn.h
 * v1(blocking)은 conn_serve_blocking, v2(non-blocking)는 conn_on_event·conn_on_timeout이
 * 같은 처리 순서를 상태(conn_t.st)로 나눠 실행한다.
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
#define CONN_MAX_WATCH 3 /* v2: 클라이언트 밖에 감시하는 fd (pipe 2 또는 업스트림 1) */

/* v2 상태 */
typedef enum {
    ST_HEAD,     /* 요청 머리 수신 */
    ST_BODY,     /* 요청 본문 수신 */
    ST_UPSTREAM, /* relay (connect·송신·응답 머리) */
    ST_CGI_HEAD, /* CGI 출력 머리 수집 */
    ST_SEND,     /* 응답 송신 */
    ST_DRAIN,    /* 에러 응답 뒤 남은 입력 버리기 */
    ST_CLOSED
} conn_state_t;

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
    bool up_reusable; /* BODY_UPSTREAM: 업스트림 응답이 연결 재사용을 허용 */

    /* ── v2 전용 ── */
    conn_state_t st;
    conn_status_t last; /* 마지막으로 돌려준 대기 상태 */
    http_request_t req; /* 처리 중인 요청 (rbuf를 빌림) */
    bool req_error; /* 에러 응답(파서 오류·408·413) 중: 송신 뒤 drain하고 닫는다 */
    bool close_after;          /* 이 응답 뒤 닫는다 */
    bool with_body;            /* 본문을 보낸다 (HEAD·204·304가 아님) */
    int64_t start_ms;          /* 요청 머리를 다 받은 시각 (접근 로그) */
    int64_t body_deadline_ms;  /* 본문 수신 마감 */
    int64_t wait_deadline_ms;  /* ST_SEND·ST_DRAIN의 현재 대기 마감 */
    uint64_t sent;             /* 보낸 본문 바이트 (접근 로그) */
    relay_t *relay;            /* ST_UPSTREAM의 상태 머신 */
    int watch[CONN_MAX_WATCH]; /* engine_watch한 추가 fd, 빈 칸 -1 */
    size_t cgi_body_off;       /* cgi_out에서 본문이 시작하는 위치 */
    off_t file_off;            /* BODY_FILE: 다음에 보낼 오프셋 */
    uint64_t body_limit;       /* BODY_PIPE: Content-Length 상한, 없으면 UINT64_MAX */
    bool body_eof;             /* BODY_PIPE·UPSTREAM: 본문 출처를 끝까지 읽었다 */
    bool up_clean;             /* BODY_UPSTREAM: 본문 뒤에 남는 바이트 없이 끝났다 */
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
static int cgi_wait_out(response_plan_t *p)
{
    int64_t deadline = p->body.pipe.deadline_ms;

    for (;;) {
        struct pollfd pfd[2] = {{p->body.pipe.out_fd, POLLIN, 0}, {p->body.pipe.in_fd, POLLOUT, 0}};
        nfds_t n = p->body.pipe.in_fd >= 0 ? 2 : 1;
        int r = poll_until(pfd, n, deadline < 0 ? INT64_MAX : deadline);

        if (r <= 0) {
            return r;
        }
        if (n == 2 && (pfd[1].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            cgi_pump_stdin(p);
        }
        if ((pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            return 1;
        }
    }
}

/* 파싱한 CGI 머리로 응답 머리를 확정한다 (v1·v2 공통). */
static void cgi_apply_head(response_plan_t *p, http_head_out_t *h, int minor)
{
    /* D-029: Content-Length가 없으면 1.1은 chunked, 1.0은 연결 종료로 끝낸다 */
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
            cgi_apply_head(p, &h, minor);
            *out_body_off = used;
            return;
        }
        buf_cleanup(&h.fields);
        if (r == HTTP_PARSE_ERROR) {
            plan_error(p, 502);
            return;
        }
        w = cgi_wait_out(p);
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
        int w = cgi_wait_out(p);
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

/* relay를 실행하고 재시도 판단을 따른다. 끝나면 plan은 BODY_UPSTREAM 또는 에러 계획. */
static void proxy_run(conn_t *c, int client_minor)
{
    const config_t *cfg = c->env->cfg;

    for (;;) {
        upstream_conn_t *uc = NULL;
        http_response_t resp;
        proxy_fail_t why = PFAIL_CONNECT;
        relay_status_t s;

        s = relay_exchange_blocking(&c->job, c->env->lim, cfg->upstream_connect_timeout_ms,
                                    cfg->upstream_response_timeout_ms, &uc, &resp, &why);
        if (s == RELAY_OK) {
            /* D-034: HTTP/1.0에 청크 구분자가 포함된 본문을 보내지 않는다. */
            if (client_minor == 0 && resp.framing.kind == FRAMING_CHUNKED) {
                upstream_release(uc, false);
                (void)plan_error(&c->plan, 502);
                return;
            }
            c->up_head_len = resp.head_len;
            c->up_reusable = resp.keep_alive;
            (void)proxy_on_head(&c->job, uc, &resp, &c->plan);
            return;
        }
        if (proxy_on_fail(&c->job, why, &c->plan) == PROXY_GIVE_UP) {
            return;
        }
    }
}

/* 업스트림 본문을 경계 추적기가 끝을 알릴 때까지 그대로 전달하고 연결을 반납한다. */
static int upstream_send_body(conn_t *c, uint64_t *sent)
{
    response_plan_t *p = &c->plan;
    upstream_conn_t *uc = p->body.upstream.conn;
    http_body_tracker_t *t = &p->body.upstream.tracker;
    const char *pre = uc->rbuf.len > c->up_head_len ? uc->rbuf.data + c->up_head_len : NULL;
    size_t pre_len = pre != NULL ? uc->rbuf.len - c->up_head_len : 0;
    bool clean = true; /* 본문 뒤에 남는 바이트 없이 끝났는가 */
    http_body_result_t res;
    size_t used = 0;
    char tmp[CONN_IO_CHUNK];
    int rc = 0;

    res = http_body_track(t, pre, pre_len, &used);
    if (send_all(c, pre, used) != 0) {
        res = BODY_ERROR;
    }
    *sent += used;
    clean = used == pre_len;
    while (res == BODY_MORE) {
        struct pollfd pfd = {uc->fd, POLLIN, 0};
        ssize_t n;

        if (poll_until(&pfd, 1, platform_now_ms() + c->env->cfg->upstream_response_timeout_ms) <=
            0) {
            res = BODY_ERROR;
            break;
        }
        n = read(uc->fd, tmp, sizeof(tmp));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (n <= 0) {
            /* CLOSE는 EOF가 본문 끝. 그 밖에는 길이 미달 */
            res = (n == 0 && t->f.kind == FRAMING_CLOSE) ? BODY_DONE : BODY_ERROR;
            clean = false;
            break;
        }
        res = http_body_track(t, tmp, (size_t)n, &used);
        if (send_all(c, tmp, used) != 0) {
            res = BODY_ERROR;
            break;
        }
        *sent += used;
        clean = used == (size_t)n;
    }
    if (res != BODY_DONE) {
        rc = -1; /* 응답이 이미 나갔으므로 에러 응답 없이 클라이언트 연결을 닫는다 */
    }
    upstream_release(uc, rc == 0 && clean && c->up_reusable && t->f.kind != FRAMING_CLOSE);
    p->body.upstream.conn = NULL;
    return rc;
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

/* 라우팅 → 처리기 (v1·v2 공통). 규칙이 없으면 404 계획으로 DONE. */
static handler_result_t dispatch_request(conn_t *c, const http_request_t *req, int64_t now_ms)
{
    const route_t *rt = router_match(c->env->router, req->host, req->path);
    handler_ctx_t ctx;

    if (rt == NULL) {
        plan_error(&c->plan, 404);
        return HANDLER_DONE;
    }
    ctx.cfg = c->env->cfg;
    ctx.lim = c->env->lim;
    ctx.client_ip = str_from_cstr(c->ip);
    ctx.server_port = c->server_port;
    ctx.now = platform_wall_time();
    ctx.now_ms = now_ms;
    return handler_dispatch(req, rt, &ctx, &c->job, &c->plan);
}

/* 계획을 만든다: 라우팅 → 처리기 → (프록시 루프) → (CGI 머리). */
static void build_plan(conn_t *c, const http_request_t *req, size_t *out_cgi_body_off)
{
    *out_cgi_body_off = 0;
    if (dispatch_request(c, req, platform_now_ms()) == HANDLER_NEED_UPSTREAM) {
        proxy_run(c, req->minor);
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
    rc = send_response(c, &req, cgi_body_off, &sent);
    fk = c->plan.head.framing.kind;
    /* response_write_head가 Connection: close를 넣는 경우와 같게 닫는다 */
    close_after = close_after || rc != 0 || c->plan.head.close || fk == FRAMING_CLOSE ||
                  (fk == FRAMING_CHUNKED && req.minor == 0);

    copy_cstr(req.method_raw, method, sizeof(method));
    copy_cstr(req.target, target, sizeof(target));
    log_access(c->ip, method, target, c->plan.head.status, sent, platform_now_ms() - start);
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

/* ── v2: non-blocking 상태 머신 ─────────────────────────────────────────── */

/* 진행 결과: 상태를 바꿨으니 이어서 진행(STEP_AGAIN) 또는 기다릴 것 */
#define STEP_AGAIN ((conn_status_t) - 1)

static void watch_fd(conn_t *c, int fd, unsigned events)
{
    int free_slot = -1;

    for (int i = 0; i < CONN_MAX_WATCH; i++) {
        if (c->watch[i] == fd) {
            (void)engine_watch(c->env->engine, fd, events, c);
            return;
        }
        if (c->watch[i] < 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0 && engine_watch(c->env->engine, fd, events, c) == 0) {
        c->watch[free_slot] = fd;
    }
}

static void unwatch_fd(conn_t *c, int fd)
{
    for (int i = 0; i < CONN_MAX_WATCH; i++) {
        if (c->watch[i] == fd && fd >= 0) {
            (void)engine_unwatch(c->env->engine, fd);
            c->watch[i] = -1;
        }
    }
}

static void unwatch_all(conn_t *c)
{
    for (int i = 0; i < CONN_MAX_WATCH; i++) {
        if (c->watch[i] >= 0) {
            (void)engine_unwatch(c->env->engine, c->watch[i]);
            c->watch[i] = -1;
        }
    }
}

/* 에러 응답(파서 오류·408·413)을 준비한다. 송신이 끝나면 drain하고 닫는다. */
static conn_status_t v2_error(conn_t *c, int status, int minor, int64_t now);
static conn_status_t v2_begin_send(conn_t *c, int64_t now);

static conn_status_t v2_head(conn_t *c, int64_t now)
{
    for (;;) {
        int st = 0;
        http_parse_result_t r =
            request_parse_head(c->rbuf.data, c->rbuf.len, c->env->lim, &c->req, &st);
        ssize_t n;

        if (r == HTTP_PARSE_DONE) {
            c->start_ms = now;
            c->body_deadline_ms = now + c->env->cfg->body_timeout_ms;
            c->st = ST_BODY;
            return STEP_AGAIN;
        }
        if (r == HTTP_PARSE_ERROR) {
            if (st == 431 && binary_start_line(&c->rbuf)) {
                return CONN_CLOSE;
            }
            return v2_error(c, st, 1, now);
        }
        n = buf_read_fd(&c->rbuf, c->fd);
        if (n > 0) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return CONN_WANT_READ;
        }
        return CONN_CLOSE; /* EOF·오류: 응답할 상대가 없다 */
    }
}

/* 요청을 처리기에 넘긴다. 결과에 따라 relay, CGI 머리 수집, 송신으로 간다. */
static conn_status_t v2_dispatch(conn_t *c, int64_t now)
{
    const config_t *cfg = c->env->cfg;
    response_plan_t *p = &c->plan;

    c->n_requests++;
    c->close_after = !c->req.keep_alive || c->n_requests >= cfg->max_keepalive_requests;
    c->req_error = false;
    c->sent = 0;
    plan_init(p);
    memset(&c->job, 0, sizeof(c->job));
    if (dispatch_request(c, &c->req, now) == HANDLER_NEED_UPSTREAM) {
        if (c->env->engine == NULL) {
            plan_error(p, 500); /* D-025 */
            return v2_begin_send(c, now);
        }
        c->relay = relay_start(&c->job, c->env->lim, now, cfg->upstream_connect_timeout_ms,
                               cfg->upstream_response_timeout_ms);
        if (c->relay == NULL) {
            plan_error(p, 500);
            return v2_begin_send(c, now);
        }
        c->st = ST_UPSTREAM;
        return STEP_AGAIN;
    }
    if (p->kind == BODY_PIPE && p->head_pending) {
        if (c->env->engine == NULL ||
            buf_init(&c->cgi_out, 4096, cfg->cgi_max_head + CONN_IO_CHUNK) != 0 ||
            platform_set_nonblock(p->body.pipe.out_fd) != 0 ||
            (p->body.pipe.in_fd >= 0 && platform_set_nonblock(p->body.pipe.in_fd) != 0)) {
            plan_error(p, 500); /* D-025 */
            return v2_begin_send(c, now);
        }
        if (p->body.pipe.in_data.len == 0 && p->body.pipe.in_fd >= 0) {
            close(p->body.pipe.in_fd);
            p->body.pipe.in_fd = -1;
        }
        c->st = ST_CGI_HEAD;
        return STEP_AGAIN;
    }
    return v2_begin_send(c, now);
}

static conn_status_t v2_body(conn_t *c, int64_t now)
{
    http_request_t *req = &c->req;
    size_t need;
    int st = 0;

    if (req->framing.kind == FRAMING_LENGTH && req->framing.length > 0) {
        if (req->framing.length > c->rbuf.max - req->head_len) {
            return v2_error(c, 413, req->minor, now);
        }
        need = req->head_len + (size_t)req->framing.length;
        while (c->rbuf.len < need) {
            ssize_t n = buf_read_fd(&c->rbuf, c->fd);

            if (n > 0) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return CONN_WANT_READ;
            }
            return CONN_CLOSE;
        }
        /* 읽는 동안 rbuf가 재할당됐을 수 있다. 같은 머리를 다시 파싱해 str_t를 새로 잡는다 */
        if (request_parse_head(c->rbuf.data, req->head_len, c->env->lim, req, &st) !=
            HTTP_PARSE_DONE) {
            return v2_error(c, 500, req->minor, now);
        }
        req->body.ptr = c->rbuf.data + req->head_len;
        req->body.len = (size_t)req->framing.length;
    }
    return v2_dispatch(c, now);
}

/* relay를 한 단계 진행한다. 재연결(D-011)로 fd가 바뀔 수 있어 감시를 매번 다시 건다. */
static conn_status_t v2_upstream(conn_t *c, unsigned events, int64_t now)
{
    int old_fd = relay_fd(c->relay);
    relay_status_t s = relay_step(c->relay, events, now);
    upstream_conn_t *uc = NULL;
    http_response_t resp;
    proxy_fail_t why = PFAIL_CONNECT;

    unwatch_fd(c, old_fd);
    if (s == RELAY_WANT_READ || s == RELAY_WANT_WRITE) {
        watch_fd(c, relay_fd(c->relay), s == RELAY_WANT_READ ? EV_READ : EV_WRITE);
        return CONN_WAIT_FD;
    }
    relay_take(c->relay, &uc, &resp, &why);
    relay_free(c->relay);
    c->relay = NULL;
    if (s == RELAY_OK) {
        /* D-034: HTTP/1.0에 청크 구분자가 포함된 본문을 보내지 않는다. */
        if (c->req.minor == 0 && resp.framing.kind == FRAMING_CHUNKED) {
            upstream_release(uc, false);
            (void)plan_error(&c->plan, 502);
            return v2_begin_send(c, now);
        }
        c->up_head_len = resp.head_len;
        c->up_reusable = resp.keep_alive;
        (void)proxy_on_head(&c->job, uc, &resp, &c->plan);
        return v2_begin_send(c, now);
    }
    if (proxy_on_fail(&c->job, why, &c->plan) == PROXY_GIVE_UP) {
        return v2_begin_send(c, now);
    }
    c->relay = relay_start(&c->job, c->env->lim, now, c->env->cfg->upstream_connect_timeout_ms,
                           c->env->cfg->upstream_response_timeout_ms);
    if (c->relay == NULL) {
        plan_error(&c->plan, 500);
        return v2_begin_send(c, now);
    }
    return STEP_AGAIN;
}

/* CGI 자식 stdin에 쓸 것이 있으면 쓰고, 다 쓰면(또는 자식이 닫으면) 감시를 끊고 닫는다. */
static void v2_cgi_stdin(conn_t *c)
{
    response_plan_t *p = &c->plan;
    ssize_t w;

    if (p->body.pipe.in_fd < 0) {
        return;
    }
    w = buf_write_fd(&p->body.pipe.in_data, p->body.pipe.in_fd);
    if ((w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || p->body.pipe.in_data.len == 0) {
        unwatch_fd(c, p->body.pipe.in_fd);
        close(p->body.pipe.in_fd);
        p->body.pipe.in_fd = -1;
    }
}

/* CGI 출력을 기다린다: stdout 읽기, 남은 stdin 쓰기. */
static conn_status_t v2_wait_pipe(conn_t *c)
{
    const response_plan_t *p = &c->plan;

    watch_fd(c, p->body.pipe.out_fd, EV_READ);
    if (p->body.pipe.in_fd >= 0) {
        watch_fd(c, p->body.pipe.in_fd, EV_WRITE);
    }
    return CONN_WAIT_FD;
}

/* CGI 실패: pipe 감시를 끊고 에러 계획으로 바꾼다 (plan_cleanup이 자식을 회수). */
static conn_status_t v2_cgi_fail(conn_t *c, int status, int64_t now)
{
    unwatch_all(c);
    plan_error(&c->plan, status);
    return v2_begin_send(c, now);
}

static conn_status_t v2_cgi_head(conn_t *c, int64_t now)
{
    response_plan_t *p = &c->plan;
    const config_t *cfg = c->env->cfg;

    for (;;) {
        http_head_out_t h;
        size_t used = 0;
        int st = 0;
        http_parse_result_t r;
        ssize_t n;

        v2_cgi_stdin(c);
        memset(&h, 0, sizeof(h));
        r = cgi_parse_head(c->cgi_out.data, c->cgi_out.len, cfg->cgi_max_head, &h, &used, &st);
        if (r == HTTP_PARSE_DONE) {
            cgi_apply_head(p, &h, c->req.minor);
            c->cgi_body_off = used;
            return v2_begin_send(c, now);
        }
        buf_cleanup(&h.fields);
        if (r == HTTP_PARSE_ERROR) {
            return v2_cgi_fail(c, 502, now);
        }
        n = buf_read_fd(&c->cgi_out, p->body.pipe.out_fd);
        if (n > 0) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return v2_wait_pipe(c);
        }
        return v2_cgi_fail(c, 502, now); /* 머리 전에 EOF 또는 오류 */
    }
}

/* 본문 조각을 송신 버퍼에 framing대로 붙인다. */
static int v2_put_body(conn_t *c, const char *p, size_t n, bool chunked)
{
    if (n == 0) {
        return 0;
    }
    c->sent += n;
    return chunked ? response_write_chunk(p, n, &c->wbuf) : buf_append(&c->wbuf, p, n);
}

/* 머리와 본문 앞부분을 송신 버퍼에 담고 송신 상태로 간다. */
static conn_status_t v2_begin_send(conn_t *c, int64_t now)
{
    response_plan_t *p = &c->plan;

    if (c->close_after) {
        p->head.close = true; /* 그 응답에 Connection: close */
    }
    c->with_body = c->req_error ||
                   (c->req.method != HTTP_HEAD && !response_status_has_no_body(p->head.status));
    c->body_eof = false;
    c->up_clean = true;
    c->wait_deadline_ms = now + c->env->cfg->body_timeout_ms;
    c->st = ST_SEND;
    buf_reset(&c->wbuf);
    if (response_write_head(&p->head, c->req.minor, platform_wall_time(), &c->wbuf) != 0) {
        return CONN_CLOSE;
    }
    switch (p->kind) {
    case BODY_MEM:
        if (c->with_body && v2_put_body(c, p->body.mem.data.data, p->body.mem.data.len, false)) {
            return CONN_CLOSE;
        }
        break;
    case BODY_FILE:
        c->file_off = p->body.file.off;
        break;
    case BODY_PIPE: {
        size_t pre = c->cgi_out.len - c->cgi_body_off;

        c->body_limit =
            p->head.framing.kind == FRAMING_LENGTH ? p->head.framing.length : UINT64_MAX;
        if (pre > c->body_limit) {
            pre = (size_t)c->body_limit;
        }
        if (c->with_body && v2_put_body(c, c->cgi_out.data + c->cgi_body_off, pre,
                                        p->head.framing.kind == FRAMING_CHUNKED) != 0) {
            return CONN_CLOSE;
        }
        break;
    }
    case BODY_UPSTREAM: {
        upstream_conn_t *uc = p->body.upstream.conn;
        const char *pre = uc->rbuf.data + c->up_head_len;
        size_t pre_len = uc->rbuf.len - c->up_head_len;
        size_t used = 0;
        http_body_result_t res;

        if (!c->with_body && p->body.upstream.tracker.f.kind != FRAMING_NONE) {
            c->body_eof = true; /* HEAD: 본문을 읽지 않는다 (plan_cleanup이 release(false)) */
            c->up_clean = false;
            break;
        }
        res = http_body_track(&p->body.upstream.tracker, pre, pre_len, &used);
        if (res == BODY_ERROR || v2_put_body(c, pre, used, false) != 0) {
            return CONN_CLOSE;
        }
        c->up_clean = used == pre_len;
        c->body_eof = res == BODY_DONE;
        break;
    }
    case BODY_NONE:
    default:
        break;
    }
    return STEP_AGAIN;
}

/* 송신 버퍼가 비었을 때 다음 본문 조각을 채운다. 1 채움, 0 본문 끝, 대기 상태면 그 값. */
static int v2_fill(conn_t *c, conn_status_t *out_wait, int64_t now)
{
    response_plan_t *p = &c->plan;
    char tmp[CONN_IO_CHUNK];
    ssize_t n;

    if (!c->with_body) {
        return 0;
    }
    switch (p->kind) {
    case BODY_PIPE: {
        bool chunked = p->head.framing.kind == FRAMING_CHUNKED;

        v2_cgi_stdin(c);
        if (c->body_eof || c->sent >= c->body_limit) {
            if (c->body_limit != UINT64_MAX && c->sent < c->body_limit) {
                return -1; /* Content-Length보다 짧게 끝났다 */
            }
            if (chunked && !c->req_error && p->body.pipe.out_fd >= 0) {
                unwatch_fd(c, p->body.pipe.out_fd);
                close(p->body.pipe.out_fd); /* 마지막 청크는 한 번만 */
                p->body.pipe.out_fd = -1;
                return response_write_last_chunk(&c->wbuf) == 0 ? 1 : -1;
            }
            return 0;
        }
        n = read(p->body.pipe.out_fd, tmp, sizeof(tmp));
        if (n == 0) {
            c->body_eof = true;
            return v2_fill(c, out_wait, now);
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                c->wait_deadline_ms = p->body.pipe.deadline_ms;
                *out_wait = v2_wait_pipe(c);
                return 2;
            }
            return -1;
        }
        if ((uint64_t)n > c->body_limit - c->sent) {
            n = (ssize_t)(c->body_limit - c->sent);
        }
        return v2_put_body(c, tmp, (size_t)n, chunked) == 0 ? 1 : -1;
    }
    case BODY_UPSTREAM: {
        upstream_conn_t *uc = p->body.upstream.conn;
        http_body_tracker_t *t = &p->body.upstream.tracker;
        size_t used = 0;
        http_body_result_t res;

        if (c->body_eof) {
            return 0;
        }
        n = read(uc->fd, tmp, sizeof(tmp));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            /* D-033: 업스트림 본문은 마지막 진행 뒤 upstream_response_timeout_ms */
            c->wait_deadline_ms = now + c->env->cfg->upstream_response_timeout_ms;
            watch_fd(c, uc->fd, EV_READ);
            *out_wait = CONN_WAIT_FD;
            return 2;
        }
        if (n <= 0) {
            /* CLOSE는 EOF가 본문 끝. 그 밖에는 길이 미달 */
            c->up_clean = false;
            if (n == 0 && t->f.kind == FRAMING_CLOSE) {
                c->body_eof = true;
                return 0;
            }
            return -1;
        }
        res = http_body_track(t, tmp, (size_t)n, &used);
        if (res == BODY_ERROR || v2_put_body(c, tmp, used, false) != 0) {
            return -1;
        }
        c->up_clean = used == (size_t)n;
        c->body_eof = res == BODY_DONE;
        return 1;
    }
    case BODY_FILE: {
        off_t end = p->body.file.off + p->body.file.len;
        off_t before = c->file_off;
        ssize_t w;

        if (c->file_off >= end) {
            return 0;
        }
        w = platform_sendfile(c->fd, p->body.file.fd, &c->file_off, (size_t)(end - c->file_off));
        if (c->file_off > before) {
            c->sent += (uint64_t)(c->file_off - before);
            c->wait_deadline_ms = now + c->env->cfg->body_timeout_ms;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            *out_wait = CONN_WANT_WRITE;
            return 2;
        }
        if (w < 0 || (w == 0 && c->file_off == before)) {
            return -1; /* 오류 또는 파일이 줄었다 */
        }
        return 1;
    }
    case BODY_MEM:
    case BODY_NONE:
    default:
        return 0;
    }
}

/* 요청 하나를 마친다: 로그, 자원 정리, 연결 판정. */
static conn_status_t v2_finish(conn_t *c, bool ok, int64_t now)
{
    response_plan_t *p = &c->plan;
    http_framing_kind_t fk = p->head.framing.kind;
    bool close_now;
    char method[16];
    char target[CONN_LOG_TARGET_MAX];

    unwatch_all(c);
    if (p->kind == BODY_UPSTREAM && p->body.upstream.conn != NULL) {
        bool reuse = ok && c->body_eof && c->up_clean && c->up_reusable &&
                     p->body.upstream.tracker.f.kind != FRAMING_CLOSE;

        upstream_release(p->body.upstream.conn, reuse);
        p->body.upstream.conn = NULL;
    }
    if (c->req_error) {
        log_access(c->ip, "-", "-", p->head.status, c->sent, 0);
    } else {
        copy_cstr(c->req.method_raw, method, sizeof(method));
        copy_cstr(c->req.target, target, sizeof(target));
        log_access(c->ip, method, target, p->head.status, c->sent, now - c->start_ms);
    }
    close_now = c->close_after || !ok || p->head.close || fk == FRAMING_CLOSE ||
                (fk == FRAMING_CHUNKED && c->req.minor == 0);
    proxy_job_cleanup(&c->job);
    plan_cleanup(&c->plan);
    buf_cleanup(&c->cgi_out);
    if (c->req_error && ok) {
        shutdown(c->fd, SHUT_WR); /* 남은 입력을 잠깐 버려 RST로 응답이 유실되지 않게 */
        c->wait_deadline_ms = now + CONN_DRAIN_MS;
        c->st = ST_DRAIN;
        return STEP_AGAIN;
    }
    if (close_now) {
        return CONN_CLOSE;
    }
    /* 처리한 요청만 지운다. 파이프라이닝으로 남은 바이트는 다음 요청이다 */
    buf_consume(&c->rbuf, c->req.head_len + c->req.body.len);
    c->head_deadline_ms = now + c->env->cfg->header_timeout_ms;
    c->idle_deadline_ms = now + c->env->cfg->keepalive_timeout_ms;
    c->st = ST_HEAD;
    return STEP_AGAIN;
}

static conn_status_t v2_send(conn_t *c, int64_t now)
{
    for (;;) {
        conn_status_t wait = CONN_WANT_WRITE;
        int r;

        if (c->wbuf.len > 0) {
            ssize_t w = buf_write_fd(&c->wbuf, c->fd);

            if (w > 0) {
                c->wait_deadline_ms = now + c->env->cfg->body_timeout_ms;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 백프레셔: 클라이언트가 막히면 본문 출처를 읽지 않는다 */
                for (int i = 0; i < CONN_MAX_WATCH; i++) {
                    if (c->watch[i] >= 0) {
                        (void)engine_watch(c->env->engine, c->watch[i], 0, c);
                    }
                }
                return CONN_WANT_WRITE;
            }
            return v2_finish(c, false, now);
        }
        r = v2_fill(c, &wait, now);
        if (r == 1) {
            continue;
        }
        if (r == 2) {
            return wait;
        }
        return v2_finish(c, r == 0, now);
    }
}

static conn_status_t v2_drain(conn_t *c)
{
    char tmp[4096];

    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof(tmp));

        if (n > 0) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return CONN_WANT_READ;
        }
        return CONN_CLOSE;
    }
}

static conn_status_t v2_error(conn_t *c, int status, int minor, int64_t now)
{
    plan_cleanup(&c->plan);
    plan_init(&c->plan);
    c->req_error = true;
    c->req.minor = minor; /* 머리를 못 읽었으면 1 (v1 respond_error와 같다) */
    c->close_after = true;
    c->sent = 0;
    if (plan_error(&c->plan, status) != 0) {
        return CONN_CLOSE;
    }
    c->plan.head.close = true;
    return v2_begin_send(c, now);
}

/* 막힐 때까지 진행한다. fd·events는 relay connect 판정에만 쓴다. */
static conn_status_t v2_advance(conn_t *c, int fd, unsigned events, int64_t now)
{
    conn_status_t s;

    do {
        switch (c->st) {
        case ST_HEAD:
            s = v2_head(c, now);
            break;
        case ST_BODY:
            s = v2_body(c, now);
            break;
        case ST_UPSTREAM:
            s = v2_upstream(c, fd == relay_fd(c->relay) ? events : 0, now);
            events = 0;
            break;
        case ST_CGI_HEAD:
            s = v2_cgi_head(c, now);
            break;
        case ST_SEND:
            s = v2_send(c, now);
            break;
        case ST_DRAIN:
            s = v2_drain(c);
            break;
        case ST_CLOSED:
        default:
            s = CONN_CLOSE;
            break;
        }
    } while (s == STEP_AGAIN);
    if (s == CONN_CLOSE) {
        c->st = ST_CLOSED;
    }
    c->last = s;
    return s;
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
    for (int i = 0; i < CONN_MAX_WATCH; i++) {
        c->watch[i] = -1;
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
    c->st = ST_HEAD;
    c->last = CONN_WANT_READ;
    return c;
}

void conn_free(conn_t *c)
{
    int e = errno;

    if (c == NULL) {
        return;
    }
    unwatch_all(c); /* 닫기 전에 감시를 끊는다 */
    relay_free(c->relay);
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
    return v2_advance(c, fd, events, now_ms);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    int64_t dl = conn_deadline_ms(c);

    if (dl < 0 || now_ms < dl) {
        return c->last;
    }
    switch (c->st) {
    case ST_HEAD:
        /* 받은 바이트가 없으면(유휴) 응답 없이 닫는다 */
        c->last = c->rbuf.len == 0 ? CONN_CLOSE : v2_error(c, 408, 1, now_ms);
        break;
    case ST_BODY:
        c->last = v2_error(c, 408, c->req.minor, now_ms);
        break;
    case ST_UPSTREAM:
        break; /* relay_step이 마감을 보고 PFAIL_CONNECT·TIMEOUT으로 끝낸다 */
    case ST_CGI_HEAD:
        c->last = v2_cgi_fail(c, 504, now_ms); /* plan_cleanup이 자식을 SIGKILL·waitpid */
        break;
    case ST_SEND:
        c->last = v2_finish(c, false, now_ms); /* 머리를 보낸 뒤의 정체: 닫는다 */
        break;
    case ST_DRAIN:
    case ST_CLOSED:
    default:
        c->last = CONN_CLOSE;
        break;
    }
    if (c->last == CONN_CLOSE) {
        c->st = ST_CLOSED;
        return CONN_CLOSE;
    }
    return v2_advance(c, -1, 0, now_ms);
}

int conn_fd(const conn_t *c)
{
    return c->fd;
}

int64_t conn_deadline_ms(const conn_t *c)
{
    switch (c->st) {
    case ST_HEAD:
        return (c->rbuf.len == 0 && c->idle_deadline_ms >= 0) ? c->idle_deadline_ms
                                                              : c->head_deadline_ms;
    case ST_BODY:
        return c->body_deadline_ms;
    case ST_UPSTREAM:
        return c->relay != NULL ? relay_deadline_ms(c->relay) : -1;
    case ST_CGI_HEAD:
        return c->plan.body.pipe.deadline_ms;
    case ST_SEND:
    case ST_DRAIN:
        return c->wait_deadline_ms;
    case ST_CLOSED:
    default:
        return -1;
    }
}
