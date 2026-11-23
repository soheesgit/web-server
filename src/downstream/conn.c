/* src/downstream/conn.c
 *
 * 클라이언트 연결 상태 머신. 명세: conn.h
 */
#include "downstream/conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common/http_framing.h"
#include "common/http_status.h"
#include "core/log.h"
#include "core/platform.h"
#include "downstream/request_parser.h"
#include "downstream/response_writer.h"
#include "handler/handler.h"
#include "handler/plan.h"
#include "upstream/relay.h"
#include "upstream/upstream.h"

/*
 * 이 파일의 역할: 요청 수신부터 계획 실행, 응답 송신, keep-alive 판정까지 연결 하나를 조정한다.
 * 구현 상태: v1 blocking 경로는 CGI pipe와 upstream relay 본문까지 연결한다. v2 이벤트 경로는
 * 메모리/파일 응답을 처리하지만 CGI와 proxy의 비동기 fd 상태 머신은 미완성이다. 그 요청은 500을 보낸다.
 * 의존 상태: 이 파일은 core/router/handler/upstream/engine 공개 API를 호출한다. 현재 저장소의
 * 해당 디렉터리에는 스텁이 남아 있어, 그 부분까지 구현해야 서버 실행이 이어진다.
 */
/* 현재 연결 단계: 요청 받기, 응답 보내기, 닫힘. */
typedef enum { CONN_READING, CONN_WRITING, CONN_FINISHED } conn_phase_t;

/*
 * 연결 하나가 소유하는 버퍼·응답 계획과 요청 처리 상태.
 * rbuf는 req의 문자열 뷰가 살아 있는 동안 consume/재할당하지 않는다.
 */
struct conn {
    int fd;                         /* 클라이언트 소켓. 연결 정리 때 닫는다. */
    struct sockaddr_storage peer;   /* accept 단계에서 받은 상대 주소 사본 */
    bool have_peer;                 /* peer 주소가 있는지 */
    char client_ip[INET6_ADDRSTRLEN]; /* 접근 로그와 처리기 문맥에 넘길 주소 문자열 */
    conn_env_t env;                 /* 서버 설정·라우터·엔진을 빌린 실행 문맥 */
    buf_t rbuf;                     /* 요청 머리·본문·파이프라인 다음 요청을 보관 */
    buf_t wbuf;                     /* 아직 클라이언트에 쓰지 못한 응답 바이트 */
    buf_t pipe_buf;                 /* CGI 출력에서 읽은 머리와 본문 앞부분 */
    http_request_t req;             /* 현재 요청. 문자열 뷰는 rbuf를 빌린다. */
    response_plan_t plan;           /* 현재 응답의 머리·본문 출처와 소유 자원 */
    proxy_job_t job;                /* 현재 요청이 프록시일 때 재시도 상태 */
    conn_phase_t phase;             /* 연결 상태 머신의 현재 단계 */
    int64_t deadline_ms;            /* 현재 읽기·쓰기 단계의 단조 시계 마감 */
    int64_t request_start_ms;       /* 접근 로그 처리 시간의 시작점 */
    size_t request_bytes;           /* 현재 요청이 rbuf에서 차지하는 총 길이 */
    size_t body_pos;                /* BODY_MEM 전송에서 다음에 보낼 위치 */
    off_t file_off;                 /* BODY_FILE 전송에서 다음 파일 오프셋 */
    uint64_t response_bytes;        /* 로그에 기록할 실제 응답 본문 바이트 */
    uint64_t request_count;         /* 이 연결에서 완료한 요청 수 */
    bool head_ready;                /* 미완성/미사용: 현재 코드에서는 읽지 않는 보조 플래그 */
    bool force_close;               /* 현재 응답 뒤 연결을 닫아야 하는지 */
    bool parser_error;              /* 기록용 상태. 현재 코드에서는 이후 분기에 사용하지 않는다. */
    bool drain_after_write;         /* 오류 응답 뒤 입력을 비워 RST를 줄인다. */
    bool upstream_keep_alive;       /* 백엔드 응답이 연결 재사용을 허용했는지 */
    bool plan_ready;                /* plan_init이 끝나 plan_cleanup이 가능한지 */
    bool body_deadline_set;         /* 본문 마감을 한 번만 계산했는지 */
    bool header_deadline_set;       /* 헤더 마감을 한 번만 계산했는지 */
    http_response_t upstream_resp;  /* relay 응답 머리 사본. 현재는 보관 후 별도 처리에 쓰지 않는다. */
};

/* 역할: 설정된 상대 timeout을 deadline 단조 시각으로 바꾼다. 음수/넘침이면 마감 없음(-1)이다. */
static int64_t add_timeout(int64_t now_ms, int timeout_ms)
{
    if (timeout_ms <= 0 || now_ms > INT64_MAX - timeout_ms) {
        return -1;
    }
    return now_ms + timeout_ms;
}

/* 역할: rbuf에 현재 요청 최대 크기와 다음 요청의 머리를 담을 상한을 계산한다. */
static size_t input_limit(const conn_env_t *env)
{
    uint64_t body = env->lim->max_body;
    size_t head = env->lim->max_head;
    if (body > (uint64_t)(SIZE_MAX - head)) {
        return SIZE_MAX;
    }
    /* 현재 요청 전체와 파이프라인 다음 요청의 머리까지 담을 여유를 둔다. */
    size_t request_max = head + (size_t)body;
    if (request_max > SIZE_MAX - head) {
        return SIZE_MAX;
    }
    return request_max + head;
}

/* 역할: peer sockaddr를 로그/처리기에서 쓸 숫자 IP 문자열로 변환한다. */
static void save_client_ip(conn_t *c)
{
    const void *addr = NULL;
    /* sockaddr 종류에 맞춰 숫자 주소를 만들고, 변환할 수 없으면 '-'를 쓴다. */
    if (!c->have_peer) {
        (void)snprintf(c->client_ip, sizeof(c->client_ip), "-");
        return;
    }
    if (c->peer.ss_family == AF_INET) {
        addr = &((const struct sockaddr_in *)&c->peer)->sin_addr;
    } else if (c->peer.ss_family == AF_INET6) {
        addr = &((const struct sockaddr_in6 *)&c->peer)->sin6_addr;
    }
    if (addr == NULL || inet_ntop(c->peer.ss_family, addr, c->client_ip, sizeof(c->client_ip)) == NULL) {
        (void)snprintf(c->client_ip, sizeof(c->client_ip), "-");
    }
}

/* 역할: 완료한 요청의 소유 자원을 정리하고 같은 연결의 다음 요청을 준비한다. */
static void clear_request_state(conn_t *c)
{
    /* 요청 처리가 끝난 뒤 계획·프록시 작업을 해제하고 다음 요청용 상태로 되돌린다. */
    plan_cleanup(&c->plan);
    proxy_job_cleanup(&c->job);
    memset(&c->req, 0, sizeof(c->req));
    memset(&c->upstream_resp, 0, sizeof(c->upstream_resp));
    buf_reset(&c->wbuf);
    buf_reset(&c->pipe_buf);
    c->request_bytes = 0;
    c->body_pos = 0;
    c->file_off = 0;
    c->response_bytes = 0;
    c->head_ready = false;
    c->force_close = false;
    c->parser_error = false;
    c->upstream_keep_alive = false;
    c->body_deadline_set = false;
    c->header_deadline_set = false;
    c->plan_ready = false;
    plan_init(&c->plan);
    c->phase = CONN_READING;
}

/* 역할: 상태 코드로 에러 응답 계획을 만들고 이 연결을 닫도록 표시한다. */
static int set_error_plan(conn_t *c, int status, bool parser_error)
{
    /* 오류도 일반 응답 계획으로 바꿔 같은 직렬화·송신 경로를 사용한다. */
    if (!c->plan_ready) {
        plan_init(&c->plan);
        c->plan_ready = true;
    }
    if (plan_error(&c->plan, status) != 0) {
        return -1;
    }
    c->plan.head.close = true;
    c->force_close = true;
    c->parser_error = parser_error;
    c->drain_after_write = parser_error || status == 408;
    return 0;
}

/* 역할: poll로 fd 준비 상태를 기다리되 절대 deadline 뒤에는 timeout 오류를 돌려준다. */
static int wait_fd(int fd, short events, int64_t deadline_ms)
{
    struct pollfd pfd;
    int timeout;
    int64_t now = platform_now_ms();
    int rc;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    if (deadline_ms < 0) {
        timeout = -1;
    } else if (now < 0 || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    } else {
        int64_t remain = deadline_ms - now;
        timeout = remain > INT_MAX ? INT_MAX : (int)remain;
    }
    /* poll은 EINTR이면 다시 기다리고, 마감 시각이 지난 경우 ETIMEDOUT을 남긴다. */
    do {
        rc = poll(&pfd, 1, timeout);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (rc < 0) {
        return -1;
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/* 역할: 주어진 바이트를 부분 전송과 EAGAIN을 처리하며 클라이언트에 모두 보낸다. */
static int send_all_client(conn_t *c, const void *data, size_t len, int64_t deadline_ms)
{
    const char *p = data;
    size_t off = 0;
    /* 부분 전송을 누적한다. 소켓이 가득 차면 쓰기 준비를 기다린 뒤 이어 보낸다. */
    while (off < len) {
        ssize_t n = platform_send(c->fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd(c->fd, POLLOUT, deadline_ms) == 0) {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

/* 역할: pipe fd의 기존 플래그를 유지한 채 O_NONBLOCK을 추가한다. */
static int set_fd_nonblock(int fd)
{
    /* CGI pipe는 부모가 읽고 쓰는 동안 멈추지 않도록 O_NONBLOCK을 켠다. */
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 역할: CGI stdin 쓰기 가능과 stdout 읽기 가능 이벤트를 기다리고 준비된 바이트를 옮긴다. */
static int poll_cgi_fds(conn_t *c)
{
    struct pollfd fds[3];
    nfds_t count = 0;
    int input_index = -1;
    int output_index = -1;
    int timeout = -1;
    int64_t now = platform_now_ms();
    int client_index = -1;
    int rc;
    /* 요청 본문을 CGI stdin에 쓰고 stdout을 읽을 수 있도록 두 fd를 함께 기다린다. */
    if (c->plan.body.pipe.in_fd >= 0 && c->plan.body.pipe.in_data.len != 0) {
        input_index = (int)count;
        fds[count].fd = c->plan.body.pipe.in_fd;
        fds[count].events = POLLOUT;
        fds[count].revents = 0;
        count++;
    }
    if (c->plan.body.pipe.out_fd >= 0) {
        output_index = (int)count;
        fds[count].fd = c->plan.body.pipe.out_fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        count++;
    }
    client_index = (int)count;
    fds[count].fd = c->fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    count++;
    if (count == 0) {
        errno = EPIPE;
        return -1;
    }
    if (c->plan.body.pipe.deadline_ms >= 0) {
        if (now < 0 || now >= c->plan.body.pipe.deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        int64_t remain = c->plan.body.pipe.deadline_ms - now;
        timeout = remain > INT_MAX ? INT_MAX : (int)remain;
    }
    do {
        rc = poll(fds, count, timeout);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (rc < 0) {
        return -1;
    }
    if ((fds[client_index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    errno = EPIPE;
    return -1;
    }

    if ((fds[client_index].revents & POLLIN) != 0) {
        char byte;
        ssize_t n;

        do {
            n = recv(c->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (n < 0 && errno == EINTR);

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    /* stdin 버퍼는 일부만 쓸 수 있으므로 보낸 만큼만 consume한다. */
    if (input_index >= 0 && (fds[input_index].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
        buf_t *in = &c->plan.body.pipe.in_data;
        ssize_t n = write(c->plan.body.pipe.in_fd, in->data, in->len);
        if (n > 0) {
            buf_consume(in, (size_t)n);
            if (in->len == 0) {
                close(c->plan.body.pipe.in_fd);
                c->plan.body.pipe.in_fd = -1;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(c->plan.body.pipe.in_fd);
            c->plan.body.pipe.in_fd = -1;
            buf_reset(in);
        }
    }
    /* stdout 바이트는 CGI 머리 파서가 검사할 수 있도록 별도 버퍼에 모은다. */
    if (output_index >= 0 && (fds[output_index].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        char temp[4096];
        ssize_t n;
        do {
            n = read(c->plan.body.pipe.out_fd, temp, sizeof(temp));
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            if (buf_append(&c->pipe_buf, temp, (size_t)n) != 0) {
                return -1;
            }
        } else if (n == 0) {
            close(c->plan.body.pipe.out_fd);
            c->plan.body.pipe.out_fd = -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    return 0;
}

/* 역할: CGI 응답 머리를 파싱할 때까지 stdin 본문 전송과 stdout 읽기를 함께 진행한다. */
static int collect_cgi_head(conn_t *c)
{
    http_head_out_t parsed_head;
    size_t used = 0;
    int status = 0;
    /* CGI 머리가 완성될 때까지 요청 본문을 stdin으로 보내면서 stdout을 함께 읽는다. */
    if (c->plan.body.pipe.out_fd < 0 ||
        (c->plan.body.pipe.in_fd >= 0 && set_fd_nonblock(c->plan.body.pipe.in_fd) != 0) ||
        set_fd_nonblock(c->plan.body.pipe.out_fd) != 0) {
        return 500;
    }
    if (c->plan.body.pipe.in_data.len == 0 && c->plan.body.pipe.in_fd >= 0) {
        close(c->plan.body.pipe.in_fd);
        c->plan.body.pipe.in_fd = -1;
    }
    memset(&parsed_head, 0, sizeof(parsed_head));
    buf_reset(&c->pipe_buf);
    for (;;) {
        /* 파서는 INCOMPLETE일 수 있다. 그때는 더 읽고, DONE이면 used 뒤가 응답 본문이다. */
        http_parse_result_t result = cgi_parse_head(c->pipe_buf.data, c->pipe_buf.len,
                                                    c->env.cfg->cgi_max_head, &parsed_head, &used,
                                                    &status);
        if (result == HTTP_PARSE_DONE) {
            bool keep_close = c->plan.head.close;
            /* CGI 상태·헤더를 클라이언트 응답 계획으로 옮기고 본문 앞부분은 남긴다. */
            buf_consume(&c->pipe_buf, used);
            buf_cleanup(&c->plan.head.fields);
            c->plan.head = parsed_head;
            c->plan.head.close = keep_close;
            c->plan.head_pending = false;
            if (c->plan.head.framing.kind == FRAMING_NONE) {
                if (c->req.minor >= 1) {
                    c->plan.head.framing.kind = FRAMING_CHUNKED;
                    c->plan.head.framing.length = 0;
                } else {
                    c->plan.head.framing.kind = FRAMING_CLOSE;
                    c->plan.head.framing.length = 0;
                    c->plan.head.close = true;
                }
            }
            return 0;
        }
        if (result == HTTP_PARSE_ERROR || c->plan.body.pipe.out_fd < 0) {
            buf_cleanup(&parsed_head.fields);
            return 502;
        }
        if (poll_cgi_fds(c) != 0) {
            int saved_errno = errno;
            buf_cleanup(&parsed_head.fields);
            return saved_errno == ETIMEDOUT ? 504 : 502;
        }
    }
}

/* 역할: upstream relay를 실행하고 실패 정책이 허용하는 범위에서 다른 백엔드를 재시도한다. */
static int proxy_exchange(conn_t *c, int64_t now_ms)
{
    /* 백엔드 연결/응답 머리를 relay에 맡기고 실패하면 proxy 정책에 따라 다시 시도한다. */
    int limit = c->job.max_attempts > 0 ? c->job.max_attempts : 1;
    for (int attempt = 0; attempt < limit; attempt++) {
        upstream_conn_t *uc = NULL;
        http_response_t resp;
        proxy_fail_t fail = PFAIL_CONNECT;
        relay_status_t st = relay_exchange_blocking(
            &c->job, c->env.lim, c->env.cfg->upstream_connect_timeout_ms,
            c->env.cfg->upstream_response_timeout_ms, &uc, &resp, &fail);
        if (st == RELAY_OK) {
            if (proxy_on_head(&c->job, uc, &resp, &c->plan) != 0) {
                return 0;
            }
            c->upstream_resp = resp;
            c->upstream_keep_alive = resp.keep_alive;
            if (c->plan.kind == BODY_UPSTREAM && c->plan.body.upstream.conn != NULL) {
                buf_consume(&c->plan.body.upstream.conn->rbuf, resp.head_len);
            }
            return 0;
        }
        if (proxy_on_fail(&c->job, fail, &c->plan) == PROXY_GIVE_UP) {
            return 0;
        }
        now_ms = platform_now_ms();
        (void)now_ms;
    }
    return plan_error(&c->plan, 502);
}

/* 역할: router 결과를 handler에 넘기고 완성된 response_plan을 응답 전송 단계에 준비한다. */
static int dispatch_request(conn_t *c, int64_t now_ms)
{
    const route_t *route;
    handler_ctx_t ctx;
    handler_result_t result;

    /* 요청 경로·Host로 route를 고르고 해당 처리기에 응답 계획을 만들게 한다. */
    plan_init(&c->plan);
    c->plan_ready = true;
    route = router_match(c->env.router, c->req.host, c->req.path);
    if (route == NULL) {
        return set_error_plan(c, 404, false);
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = c->env.cfg;
    ctx.lim = c->env.lim;
    ctx.client_ip.ptr = c->client_ip;
    ctx.client_ip.len = strlen(c->client_ip);
    ctx.server_port = c->env.cfg->port;
    ctx.now = time(NULL);
    ctx.now_ms = now_ms;
    memset(&c->job, 0, sizeof(c->job));
    result = handler_dispatch(&c->req, route, &ctx, &c->job, &c->plan);
    /* 프록시 처리기만 relay 교환을 추가로 수행한다. */
    if (result == HANDLER_NEED_UPSTREAM) {
        if (c->env.nonblocking) {
            /* v2 비동기 relay 상태 머신은 아직 연결되지 않아 이 경로는 500으로 종료한다. */
            return set_error_plan(c, 500, false);
        }
        if (proxy_exchange(c, now_ms) != 0) {
            return set_error_plan(c, 502, false);
        }
    }
    /* 처리기가 유효한 상태를 만들었는지 확인하고, 응답 뒤 닫을 조건을 머리에 반영한다. */
    if (!c->plan.head_pending &&
        (c->plan.head.status < 100 || c->plan.head.status > 599)) {
        return set_error_plan(c, 500, false);
    }
    if (c->env.nonblocking && (c->plan.head_pending || c->plan.kind == BODY_PIPE ||
                            c->plan.kind == BODY_UPSTREAM)) {
        if (set_error_plan(c, 500, false) != 0) {
            return -1;
        }
    }
    if (c->plan.head_pending) {
        int cgi_status = collect_cgi_head(c);
    if (cgi_status != 0) {
        if (set_error_plan(c, cgi_status, false) != 0) {
            return -1;
        }
    }
    }
    if (!c->req.keep_alive ||
        (c->env.cfg->max_keepalive_requests > 0 &&
         c->request_count + 1 >= (uint64_t)c->env.cfg->max_keepalive_requests)) {
        c->plan.head.close = true;
    }
    if (c->force_close) {
        c->plan.head.close = true;
    }
    /* 실제 응답 머리를 write 버퍼에 만들고 이후 송신 상태로 이동한다. */
    c->file_off = c->plan.kind == BODY_FILE ? c->plan.body.file.off : 0;
    buf_reset(&c->wbuf);
    if (response_write_head(&c->plan.head, c->req.minor, time(NULL), &c->wbuf) != 0) {
        return -1;
    }
    c->phase = CONN_WRITING;
    c->deadline_ms = add_timeout(now_ms, c->env.cfg->body_timeout_ms);
    return 0;
}

/* 역할: CGI stdout 본문을 framing 규칙에 맞춰 클라이언트로 중계하고 자식을 회수한다. */
static int send_pipe_body(conn_t *c)
{
    char data[8192];
    uint64_t remaining = c->plan.head.framing.length;
    bool limited = c->plan.head.framing.kind == FRAMING_LENGTH;
    bool chunked = c->plan.head.framing.kind == FRAMING_CHUNKED;
    bool eof = false;
    /* CGI stdout을 EOF까지 읽는다. 응답 framing에 따라 원문 또는 chunk 단위로 감싼다. */
    for (;;) {
        ssize_t n;
        size_t take;
        /* 머리 파싱 때 미리 읽은 본문이 있으면 pipe보다 먼저 전송한다. */
        if (c->pipe_buf.len != 0) {
            take = c->pipe_buf.len < sizeof(data) ? c->pipe_buf.len : sizeof(data);
            if (limited && (uint64_t)take > remaining) {
                take = (size_t)remaining;
            }
            memcpy(data, c->pipe_buf.data, take);
            buf_consume(&c->pipe_buf, take);
            n = (ssize_t)take;
        } else {
            if (limited && remaining == 0) {
                break;
            }
            if (c->plan.body.pipe.out_fd < 0) {
                n = 0;
            } else {
                if (wait_fd(c->plan.body.pipe.out_fd, POLLIN, c->plan.body.pipe.deadline_ms) != 0) {
                    return -1;
                }
                do {
                    n = read(c->plan.body.pipe.out_fd, data, sizeof(data));
                } while (n < 0 && errno == EINTR);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                if (n < 0) {
                    return -1;
                }
            }
        }
        if (n == 0) {
            if (limited && remaining != 0) {
                errno = EPROTO;
                return -1;
            }
            eof = true;
            if (c->plan.body.pipe.out_fd >= 0) {
                close(c->plan.body.pipe.out_fd);
                c->plan.body.pipe.out_fd = -1;
            }
            break;
        }
        take = (size_t)n;
        /* chunked 응답은 각 데이터 블록 앞에 크기 줄을 붙인다. */
        if (chunked) {
            buf_reset(&c->wbuf);
            if (response_write_chunk(data, take, &c->wbuf) != 0 ||
                send_all_client(c, c->wbuf.data, c->wbuf.len, c->deadline_ms) != 0) {
                return -1;
            }
        } else if (send_all_client(c, data, take, c->deadline_ms) != 0) {
            return -1;
        }
        c->response_bytes += take;
        if (limited) {
            remaining -= take;
            if (remaining == 0) {
                break;
            }
        }
    }
    if (chunked) {
        buf_reset(&c->wbuf);
        if (response_write_last_chunk(&c->wbuf) != 0 ||
            send_all_client(c, c->wbuf.data, c->wbuf.len, c->deadline_ms) != 0) {
            return -1;
        }
    }
    /* stdout EOF 뒤 자식 종료 코드를 회수해 좀비 프로세스를 남기지 않는다. */
    if (eof && c->plan.body.pipe.pid > 0) {
        int child_status = 0;
        for (;;) {
            pid_t waited = waitpid(c->plan.body.pipe.pid, &child_status, WNOHANG);
            if (waited == c->plan.body.pipe.pid) {
                c->plan.body.pipe.pid = 0;
                if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
                    errno = ECHILD;
                    return -1;
                }
                break;
            }
            if (waited < 0 && errno != EINTR) {
                c->plan.body.pipe.pid = 0;
                return -1;
            }
            if (c->plan.body.pipe.deadline_ms >= 0 &&
                platform_now_ms() >= c->plan.body.pipe.deadline_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            (void)poll(NULL, 0, 10);
        }
    }
    return 0;
}

/* 역할: tracker가 현재 응답 끝을 찾을 때까지 백엔드 본문을 클라이언트로 중계한다. */
static int send_upstream_body(conn_t *c)
{
    upstream_conn_t *uc = c->plan.body.upstream.conn;
    http_body_tracker_t *tracker = &c->plan.body.upstream.tracker;
    /* 클라이언트 연결을 닫아도, 백엔드 응답이 유지 가능하면 그 연결은 풀에 돌려준다. */
    bool reusable = c->upstream_keep_alive;
    if (uc == NULL) {
        return 0;
    }
    /* 백엔드 버퍼를 body tracker에 넣어 현재 응답에 속한 바이트만 클라이언트에 전달한다. */
    for (;;) {
        size_t used = 0;
        http_body_result_t result;
        if (uc->rbuf.len == 0) {
            ssize_t n = buf_read_fd(&uc->rbuf, uc->fd);
            if (n == 0) {
                if (tracker->f.kind == FRAMING_CLOSE) {
                    upstream_release(uc, false);
                    c->plan.body.upstream.conn = NULL;
                    return 0;
                }
                errno = EPROTO;
                reusable = false;
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (wait_fd(uc->fd, POLLIN, c->deadline_ms) == 0) {
                        continue;
                    }
                }
                reusable = false;
                break;
            }
        }
        /* chunked/Content-Length 경계를 추적해 다음 응답 데이터가 섞이지 않게 한다. */
        result = http_body_track(tracker, uc->rbuf.data, uc->rbuf.len, &used);
        if (result == BODY_ERROR) {
            errno = EPROTO;
            reusable = false;
            break;
        }
        if (used != 0) {
            if (send_all_client(c, uc->rbuf.data, used, c->deadline_ms) != 0) {
                reusable = false;
                break;
            }
            c->response_bytes += used;
            buf_consume(&uc->rbuf, used);
        }
        if (result == BODY_DONE) {
            reusable = reusable && uc->rbuf.len == 0;
            upstream_release(uc, reusable);
            c->plan.body.upstream.conn = NULL;
            return 0;
        }
        if (used == 0 && uc->rbuf.len != 0) {
            errno = EPROTO;
            reusable = false;
            break;
        }
    }
    upstream_release(uc, false);
    c->plan.body.upstream.conn = NULL;
    return -1;
}

/* 역할: v1 응답 본문 종류에 따라 pipe/upstream 본문 중계기를 선택한다. */
static int send_body_blocking(conn_t *c)
{
    /* HEAD와 본문 없는 상태는 계획에 본문 자원이 있어도 송신하지 않는다. */
    if (c->req.method == HTTP_HEAD || c->plan.kind == BODY_NONE ||
        (c->plan.head.status >= 100 && c->plan.head.status < 200) || c->plan.head.status == 204 ||
        c->plan.head.status == 304) {
        return 0;
    }
    if (c->plan.kind == BODY_PIPE) {
        return send_pipe_body(c);
    }
    if (c->plan.kind == BODY_UPSTREAM) {
        return send_upstream_body(c);
    }
    return 0;
}

/* 역할: 접근 로그를 쓰고 현재 요청을 정리한 뒤 연결 종료 또는 keep-alive를 결정한다. */
static int finish_request(conn_t *c, int64_t now_ms)
{
    /* 접근 로그를 남긴 뒤 자원을 정리하고, keep-alive면 다음 요청을 시작한다. */
    bool close_after = c->plan.head.close || !c->req.keep_alive || c->force_close;
    char method[8];
    char *target = NULL;
    const char *method_arg = NULL;
    if (c->req.method_raw.ptr != NULL && c->req.method_raw.len < sizeof(method)) {
        memcpy(method, c->req.method_raw.ptr, c->req.method_raw.len);
        method[c->req.method_raw.len] = '\0';
        method_arg = method;
    }
    if (c->req.target.ptr != NULL && c->req.target.len < SIZE_MAX) {
        target = malloc(c->req.target.len + 1);
        if (target != NULL) {
            memcpy(target, c->req.target.ptr, c->req.target.len);
            target[c->req.target.len] = '\0';
        }
    }
    log_access(c->client_ip, method_arg, target, c->plan.head.status, c->response_bytes,
               now_ms >= c->request_start_ms ? now_ms - c->request_start_ms : 0);
    free(target);
    if (c->plan.kind == BODY_UPSTREAM && c->plan.body.upstream.conn != NULL) {
        upstream_release(c->plan.body.upstream.conn, false);
        c->plan.body.upstream.conn = NULL;
    }
    c->request_count++;
    size_t consumed = c->request_bytes;
    clear_request_state(c);
    if (consumed != 0) {
        buf_consume(&c->rbuf, consumed);
    }
    if (close_after) {
        c->phase = CONN_FINISHED;
        c->deadline_ms = -1;
        return 1;
    }
    c->deadline_ms = add_timeout(now_ms, c->env.cfg->keepalive_timeout_ms);
    return 0;
}

/* 역할: 응답 머리와 지원하는 본문을 가능한 만큼 보내고 다음 이벤트 종류를 반환한다. */
static conn_status_t flush_response(conn_t *c, int64_t now_ms)
{
    /* 먼저 머리 버퍼를 비운 뒤 BODY_MEM 또는 BODY_FILE을 이어서 전송한다. */
    while (c->wbuf.len != 0) {
        ssize_t n = platform_send(c->fd, c->wbuf.data, c->wbuf.len);
        if (n > 0) {
            buf_consume(&c->wbuf, (size_t)n);
            if (c->env.nonblocking) {
                continue;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return CONN_WANT_WRITE;
        }
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    if (c->env.nonblocking && c->plan.kind != BODY_NONE && c->req.method != HTTP_HEAD &&
        c->plan.kind != BODY_MEM && c->plan.kind != BODY_FILE) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    /* 메모리 본문은 body_pos부터 부분 전송하고, 파일 본문은 sendfile 오프셋을 유지한다. */
    if (c->req.method != HTTP_HEAD && c->plan.kind == BODY_MEM) {
        size_t total = c->plan.body.mem.data.len;
        while (c->body_pos < total) {
            ssize_t n = platform_send(c->fd, c->plan.body.mem.data.data + c->body_pos,
                                     total - c->body_pos);
            if (n > 0) {
                c->body_pos += (size_t)n;
                c->response_bytes += (uint64_t)n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return CONN_WANT_WRITE;
            }
            c->phase = CONN_FINISHED;
            return CONN_CLOSE;
        }
    } else if (c->req.method != HTTP_HEAD && c->plan.kind == BODY_FILE) {
        off_t end = c->plan.body.file.off + c->plan.body.file.len;
        while (c->file_off < end) {
            off_t remain = end - c->file_off;
            size_t ask = remain > 65536 ? 65536 : (size_t)remain;
            ssize_t n = platform_sendfile(c->fd, c->plan.body.file.fd, &c->file_off, ask);
            if (n > 0) {
                c->response_bytes += (uint64_t)n;
                continue;
            }
            if (n == 0) {
                c->phase = CONN_FINISHED;
                return CONN_CLOSE;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return CONN_WANT_WRITE;
            }
            c->phase = CONN_FINISHED;
            return CONN_CLOSE;
        }
    }
    if (!c->env.nonblocking) {
        if (send_body_blocking(c) != 0) {
            c->phase = CONN_FINISHED;
            return CONN_CLOSE;
        }
    }
    int done = finish_request(c, now_ms);
    if (done != 0) {
        return CONN_CLOSE;
    }
    c->request_start_ms = now_ms;
    if (c->rbuf.len != 0) {
        c->deadline_ms = add_timeout(now_ms, c->env.cfg->header_timeout_ms);
        c->header_deadline_set = true;
        return conn_on_event(c, c->fd, EV_READ, now_ms);
    }
    c->header_deadline_set = false;
    return CONN_WANT_READ;
}

/* 역할: 연결 객체/버퍼를 생성하고 blocking 설정을 적용한다. 성공 뒤 fd 소유권은 conn으로 간다. */
conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env,
                 int64_t now_ms)
{
    conn_t *c;
    size_t max_input;
    size_t pipe_max;
    int flags;
    struct timeval tv = {0, 250000};
    /* 연결 객체와 버퍼를 만들고 소켓 모드를 설정한다. 실패하면 fd 소유권은 호출자에게 남는다. */
    if (fd < 0 || env == NULL || env->cfg == NULL || env->lim == NULL || env->router == NULL) {
        errno = EINVAL;
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->fd = fd;
    c->env = *env;
    if (peer != NULL) {
        c->peer = *peer;
        c->have_peer = true;
    }
    save_client_ip(c);
    /* 수신 버퍼는 요청 최대 크기와 파이프라인 다음 요청 머리를 담을 수 있게 잡는다. */
    max_input = input_limit(env);
    if (max_input == 0) {
        max_input = 1;
    }
    if (buf_init(&c->rbuf, max_input < 4096 ? max_input : 4096, max_input) != 0 ||
        buf_init(&c->wbuf, 512, 128 * 1024) != 0) {
        buf_cleanup(&c->rbuf);
        buf_cleanup(&c->wbuf);
        free(c);
        return NULL;
    }
    pipe_max = env->cfg->cgi_max_head > SIZE_MAX - 65536 ? SIZE_MAX : env->cfg->cgi_max_head + 65536;
    if (pipe_max == 0) {
        pipe_max = 65536;
    }
    if (buf_init(&c->pipe_buf, pipe_max < 4096 ? pipe_max : 4096, pipe_max) != 0) {
        buf_cleanup(&c->rbuf);
        buf_cleanup(&c->wbuf);
        free(c);
        return NULL;
    }
    /* v2는 non-blocking, v1은 blocking 소켓에 짧은 커널 timeout을 설정한다. */
    if (env->nonblocking) {
        if (platform_set_nonblock(fd) != 0) {
            buf_cleanup(&c->rbuf);
            buf_cleanup(&c->wbuf);
            buf_cleanup(&c->pipe_buf);
            free(c);
            return NULL;
        }
    } else {
        flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
            buf_cleanup(&c->rbuf);
            buf_cleanup(&c->wbuf);
            buf_cleanup(&c->pipe_buf);
            free(c);
            return NULL;
        }
    }
    plan_init(&c->plan);
    c->phase = CONN_READING;
    c->deadline_ms = add_timeout(now_ms, env->cfg->header_timeout_ms);
    c->header_deadline_set = true;
    c->request_start_ms = now_ms;
    return c;
}

/* 역할: 현재 계획, 버퍼, 요청 작업, 클라이언트 fd를 정리한다. */
void conn_free(conn_t *c)
{
    int saved_errno = errno;
    if (c == NULL) {
        errno = saved_errno;
        return;
    }
    /* 연결이 어느 단계에서 끝났든 계획·프록시 버퍼·소켓을 모두 회수한다. */
    plan_cleanup(&c->plan);
    proxy_job_cleanup(&c->job);
    buf_cleanup(&c->pipe_buf);
    buf_cleanup(&c->wbuf);
    buf_cleanup(&c->rbuf);
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
    errno = saved_errno;
}

/* 역할: 클라이언트에서 한 번 읽어 rbuf에 붙이고 EOF/상한/오류를 구분한다. */
static ssize_t read_client_once(conn_t *c)
{
    /* core 버퍼 함수로 소켓에서 한 번 읽는다. 버퍼 상한 도달은 별도 값 -2로 구분한다. */
    ssize_t n = buf_read_fd(&c->rbuf, c->fd);
    if (n < 0) {
        if (errno == EMSGSIZE) {
            return -2;
        }
        return -1;
    }
    return n;
}

/* 역할: v2 이벤트 한 번을 처리해 다음에 읽기/쓰기/닫기 중 무엇을 할지 반환한다. */
conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms)
{
    /* v2 진입점: 이벤트가 난 fd를 처리하고 엔진에 다음 대기 상태를 알려준다. */
    if (c == NULL || c->fd < 0 || c->phase == CONN_FINISHED || fd != c->fd) {
        return CONN_CLOSE;
    }
    if ((events & EV_ERROR) != 0) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    /* 이미 버퍼에 들어온 파이프라인 요청도 읽기 이벤트를 다시 기다리지 않고 처리한다. */
    for (int step = 0; step < 8; step++) {
        if (c->phase == CONN_WRITING) {
            return flush_response(c, now_ms);
        }
        if (c->rbuf.len == 0 && (events & EV_READ) == 0) {
            return CONN_WANT_READ;
        }
        if (c->rbuf.len == 0) {
            c->request_start_ms = now_ms;
        }
        http_parse_result_t parsed;
        int status = 0;
        /* 읽은 바이트로 요청 머리를 파싱한다. DONE 전에는 라우터나 처리기를 호출하지 않는다. */
        parsed = request_parse_head(c->rbuf.data, c->rbuf.len, c->env.lim, &c->req, &status);
        if (parsed == HTTP_PARSE_ERROR) {
            if (set_error_plan(c, status, true) != 0) {
                c->phase = CONN_FINISHED;
                return CONN_CLOSE;
            }
            c->plan_ready = true;
            c->request_bytes = 0;
            c->phase = CONN_WRITING;
            buf_reset(&c->wbuf);
            if (response_write_head(&c->plan.head, 1, time(NULL), &c->wbuf) != 0) {
                c->phase = CONN_FINISHED;
                return CONN_CLOSE;
            }
            return flush_response(c, now_ms);
        }
        if (parsed == HTTP_PARSE_INCOMPLETE) {
            /* 머리가 아직 덜 왔으면 더 읽는다. 불완전한 상태에서 peer가 닫히면 400을 만든다. */
            if ((events & EV_READ) == 0) {
                return CONN_WANT_READ;
            }
            ssize_t n = read_client_once(c);
            if (n > 0) {
                if (c->request_count != 0 && !c->header_deadline_set) {
                    c->deadline_ms = add_timeout(now_ms, c->env.cfg->header_timeout_ms);
                    c->header_deadline_set = true;
                }
                if (!c->env.nonblocking) {
                    events = 0;
                }
                continue;
            }
            if (n == 0) {
                if (c->rbuf.len == 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                if (set_error_plan(c, 400, true) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                c->phase = CONN_WRITING;
                buf_reset(&c->wbuf);
                if (response_write_head(&c->plan.head, 1, time(NULL), &c->wbuf) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                return flush_response(c, now_ms);
            }
            if (n == -2) {
                if (set_error_plan(c, 431, true) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                c->phase = CONN_WRITING;
                buf_reset(&c->wbuf);
                if (response_write_head(&c->plan.head, 1, time(NULL), &c->wbuf) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                return flush_response(c, now_ms);
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return CONN_WANT_READ;
            }
            c->phase = CONN_FINISHED;
            return CONN_CLOSE;
        }

        /* 머리 뒤 본문 길이만큼 모은 뒤 req.body가 rbuf를 빌리게 한다. */
        if (c->req.framing.kind == FRAMING_LENGTH && c->req.framing.length > 0) {
            uint64_t total64 = (uint64_t)c->req.head_len + c->req.framing.length;
            if (total64 > SIZE_MAX) {
                if (set_error_plan(c, 413, true) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                c->phase = CONN_WRITING;
                buf_reset(&c->wbuf);
                if (response_write_head(&c->plan.head, c->req.minor, time(NULL), &c->wbuf) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                return flush_response(c, now_ms);
            }
            size_t total = (size_t)total64;
            if (c->rbuf.len < total) {
                if (!c->body_deadline_set) {
                    c->deadline_ms = add_timeout(now_ms, c->env.cfg->body_timeout_ms);
                    c->body_deadline_set = true;
                }
                if ((events & EV_READ) == 0) {
                    return CONN_WANT_READ;
                }
                ssize_t n = read_client_once(c);
                if (n > 0) {
                    if (!c->env.nonblocking) {
                        events = 0;
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return CONN_WANT_READ;
                }
                if (set_error_plan(c, 400, true) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                c->phase = CONN_WRITING;
                buf_reset(&c->wbuf);
                if (response_write_head(&c->plan.head, c->req.minor, time(NULL), &c->wbuf) != 0) {
                    c->phase = CONN_FINISHED;
                    return CONN_CLOSE;
                }
                return flush_response(c, now_ms);
            }
            c->req.body.ptr = c->rbuf.data + c->req.head_len;
            c->req.body.len = (size_t)c->req.framing.length;
            c->request_bytes = total;
        } else {
            c->req.body.ptr = NULL;
            c->req.body.len = 0;
            c->request_bytes = c->req.head_len;
        }
        /* 요청이 완성되면 dispatch해 응답 계획을 만들고 송신 상태로 간다. */
        c->deadline_ms = -1;
        if (dispatch_request(c, now_ms) != 0) {
            c->phase = CONN_FINISHED;
            return CONN_CLOSE;
        }
        return flush_response(c, now_ms);
    }
    return c->phase == CONN_WRITING ? CONN_WANT_WRITE : CONN_WANT_READ;
}

/* 역할: 현재 단계의 마감이 지났을 때 408 응답 또는 무응답 종료를 선택한다. */
conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    /* 타임아웃 단계에 따라 유휴 연결은 조용히 닫고, 진행 중 요청은 408로 끝낸다. */
    if (c == NULL || c->phase == CONN_FINISHED) {
        return CONN_CLOSE;
    }
    if (c->deadline_ms < 0 || now_ms < c->deadline_ms) {
        return c->phase == CONN_WRITING ? CONN_WANT_WRITE : CONN_WANT_READ;
    }
    if (c->phase == CONN_WRITING) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    if (c->rbuf.len == 0) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    if (set_error_plan(c, 408, true) != 0) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    c->phase = CONN_WRITING;
    buf_reset(&c->wbuf);
    if (response_write_head(&c->plan.head, c->req.minor < 0 ? 1 : c->req.minor, time(NULL),
                            &c->wbuf) != 0) {
        c->phase = CONN_FINISHED;
        return CONN_CLOSE;
    }
    return flush_response(c, now_ms);
}

/* 역할: 아직 살아 있는 클라이언트 fd를 조회한다. */
int conn_fd(const conn_t *c)
{
    /* 엔진이 감시할 클라이언트 fd. 닫힘 상태에서는 -1을 돌려준다. */
    if (c == NULL || c->phase == CONN_FINISHED) {
        return -1;
    }
    return c->fd;
}

/* 역할: 연결이 기다리는 단계의 다음 deadline을 조회한다. */
int64_t conn_deadline_ms(const conn_t *c)
{
    /* 엔진이 다음 timeout 콜백을 예약할 단조 시각을 돌려준다. */
    return c == NULL ? -1 : c->deadline_ms;
}

/* 역할: v1에서 한 연결의 요청/응답 처리를 닫힐 때까지 반복한다. */
void conn_serve_blocking(conn_t *c)
{
    /* v1 루프: 현재 단계에 따라 poll하고 요청/응답을 끝날 때까지 처리한다. */
    if (c == NULL || c->env.nonblocking) {
        return;
    }
    while (c->phase != CONN_FINISHED) {
        int64_t deadline = c->deadline_ms;
        int64_t now = platform_now_ms();
        int timeout = -1;
        struct pollfd pfd;
        int rc;
        if (deadline >= 0) {
            if (now >= deadline) {
                conn_status_t st = conn_on_timeout(c, now);
                if (st == CONN_CLOSE) {
                    break;
                }
                continue;
            }
            int64_t remain = deadline - now;
            timeout = remain > 1000 ? 1000 : (int)remain;
        }
        /* 요청 수신 중에는 읽기, 응답 송신 중에는 쓰기 준비를 기다린다. */
        pfd.fd = c->fd;
        pfd.events = c->phase == CONN_WRITING ? POLLOUT : POLLIN;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, timeout);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) {
            if (deadline >= 0 && platform_now_ms() >= deadline) {
                (void)conn_on_timeout(c, platform_now_ms());
            }
            continue;
        }
        if (rc < 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            break;
        }
        unsigned ev = (pfd.revents & POLLOUT) != 0 ? EV_WRITE : EV_READ;
        conn_status_t st = conn_on_event(c, c->fd, ev, platform_now_ms());
        if (st == CONN_CLOSE || st == CONN_WAIT_FD) {
            break;
        }
    }
    /* 오류 응답을 보낸 직후 입력 잔여 바이트를 잠깐 비워 RST로 응답이 사라지는 일을 줄인다. */
    if (c->phase == CONN_FINISHED && c->drain_after_write && c->fd >= 0) {
        char discard[1024];
        int64_t end = add_timeout(platform_now_ms(), 1000);
        (void)shutdown(c->fd, SHUT_WR);
        while (wait_fd(c->fd, POLLIN, end) == 0) {
            ssize_t n = read(c->fd, discard, sizeof(discard));
            if (n <= 0) {
                break;
            }
        }
    }
}
