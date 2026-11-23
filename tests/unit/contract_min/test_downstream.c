/* tests/unit/contract_min/test_downstream.c
 *
 * downstream/ 계약 최소셋. 기대값의 원래 표: 기획서 9장. ID 목록: src/downstream/README.md
 * conn 테스트(U-DS-40, 44)는 실제 router + STATIC 처리기를 쓴다 (D-018).
 */
#include "unit/testlib.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common/types.h"
#include "core/buf.h"
#include "downstream/conn.h"
#include "downstream/listener.h"
#include "downstream/request_parser.h"
#include "downstream/response_writer.h"
#include "downstream/uri.h"
#include "fakes/fake_handler.h"
#include "router/router.h"

static const http_limits_t LIM = {8192, 2048, 64, 10485760};

static http_parse_result_t parse(const char *p, http_request_t *req, int *st)
{
    memset(req, 0, sizeof(*req));
    *st = 0;
    return request_parse_head(p, strlen(p), &LIM, req, st);
}

static void t_u_ds_01(void)
{
    const char *p = "GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n";
    http_request_t req;
    int st;

    CHECK_EQ_INT(parse(p, &req, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(req.method, HTTP_GET);
    CHECK_STR_EQ(req.method_raw, "GET");
    CHECK_EQ_STR(req.path, "/index.html");
    CHECK_EQ_INT(req.minor, 1);
    CHECK_STR_EQ(req.host, "a");
    CHECK_EQ_INT(req.framing.kind, FRAMING_NONE);
    CHECK(req.keep_alive == true);
    CHECK_EQ_INT(req.head_len, strlen(p));
}

static void t_u_ds_06(void)
{
    const char *one = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *p = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    http_request_t req;
    int st = 0;
    size_t first;

    CHECK_EQ_INT(parse(p, &req, &st), HTTP_PARSE_DONE);
    CHECK_EQ_STR(req.path, "/a");
    CHECK_EQ_INT(req.head_len, strlen(one));
    first = req.head_len;
    memset(&req, 0, sizeof(req));
    CHECK_EQ_INT(request_parse_head(p + first, strlen(p) - first, &LIM, &req, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_STR(req.path, "/b");
}

static void t_u_ds_07(void)
{
    static const char *bad[] = {
        "GET /\r\nHost: a\r\n\r\n",
        "GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n",
        "GET  / HTTP/1.1\r\nHost: a\r\n\r\n",
    };
    http_request_t req;
    int st;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_parse_result_t r = parse(bad[i], &req, &st);
        if (r != HTTP_PARSE_ERROR || st != 400) {
            test_fail(__FILE__, __LINE__, "case %zu: expected ERROR 400, got %d/%d", i, (int)r, st);
            return;
        }
    }
}

static void t_u_ds_12(void)
{
    http_request_t req;
    int st;

    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\n\r\n", &req, &st), HTTP_PARSE_ERROR);
    CHECK_EQ_INT(st, 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &req, &st),
                 HTTP_PARSE_ERROR);
    CHECK_EQ_INT(st, 400);
    CHECK_EQ_INT(parse("GET / HTTP/1.0\r\n\r\n", &req, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(req.minor, 0);
}

static void t_u_ds_22(void)
{
    char out[64];
    size_t n = 0;
    str_t a = {"%20", 3};
    str_t b = {"%EA%B0%80", 9};

    CHECK_EQ_INT(uri_decode_path(a, out, sizeof(out), &n), 0);
    CHECK_EQ_MEM(out, n, " ", 1);
    CHECK_EQ_INT(uri_decode_path(b, out, sizeof(out), &n), 0);
    CHECK_EQ_MEM(out, n, "\xEA\xB0\x80", 3);
    CHECK_EQ_STR(out, "\xEA\xB0\x80");
}

static void t_u_ds_27(void)
{
    char a[] = "/a/./b//c/";
    char b[] = "/a/b/../c";
    char c[] = "/";
    char d[] = "/a/b/../../c"; /* 연속 ".."는 세그먼트를 하나씩 지운다 */
    char e[] = "/a/b/..";

    CHECK_EQ_INT(path_normalize(a), 0);
    CHECK_EQ_STR(a, "/a/b/c/");
    CHECK_EQ_INT(path_normalize(b), 0);
    CHECK_EQ_STR(b, "/a/c");
    CHECK_EQ_INT(path_normalize(c), 0);
    CHECK_EQ_STR(c, "/");
    CHECK_EQ_INT(path_normalize(d), 0);
    CHECK_EQ_STR(d, "/c");
    CHECK_EQ_INT(path_normalize(e), 0);
    CHECK_EQ_STR(e, "/a/");
}

static void t_u_ds_28(void)
{
    char ok[] = "/a/b/../c";
    char up1[] = "/..";
    char up2[] = "/a/../../b";
    char up3[] = "/a/b/../../../c";

    CHECK_EQ_INT(path_normalize(ok), 0); /* 정상 대조 (D-017) */
    CHECK_EQ_STR(ok, "/a/c");
    CHECK_EQ_INT(path_normalize(up1), -1);
    CHECK_EQ_INT(path_normalize(up2), -1);
    CHECK_EQ_INT(path_normalize(up3), -1);
}

static void t_u_ds_30(void)
{
    const char *ct = "Content-Type: text/html; charset=utf-8\r\n";
    const char *want = "HTTP/1.1 200 OK\r\n"
                       "Date: Tue, 14 Nov 2023 22:13:20 GMT\r\n"
                       "Server: httpd/0.1\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n";
    http_head_out_t head;
    buf_t out;

    memset(&head, 0, sizeof(head));
    head.status = 200;
    head.framing.kind = FRAMING_LENGTH;
    head.framing.length = 5;
    CHECK_EQ_INT(buf_init(&head.fields, 64, 4096), 0);
    CHECK_EQ_INT(buf_append(&head.fields, ct, strlen(ct)), 0);
    CHECK_EQ_INT(buf_init(&out, 256, 8192), 0);
    CHECK_EQ_INT(response_write_head(&head, 1, (time_t)1700000000, &out), 0);
    CHECK_EQ_MEM(out.data, out.len, want, strlen(want));
    buf_cleanup(&out);
    buf_cleanup(&head.fields);
}

static void t_u_ds_36(void)
{
    char err[128] = "";
    int fd = listener_open("127.0.0.1", 0, 16, err, sizeof(err));
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    if (fd < 0) {
        test_fail(__FILE__, __LINE__, "listener_open failed: %s", err);
        return;
    }
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(listener_port(fd) > 0);
    CHECK_EQ_INT(getsockname(fd, (struct sockaddr *)&sa, &sl), 0);
    CHECK_EQ_INT(ntohs(sa.sin_port), listener_port(fd));
    listener_close(fd);
}

/* ── conn 공통 ─────────────────────────────────────────────────────────── */

typedef struct {
    config_t *cfg;
    router_t *router;
    conn_env_t env;
    int sv[2];
    conn_t *c;
} conn_fixture_t;

/* conn_setup이 만든 router. 테스트가 해제하지 않으므로 도달 가능하게 두어 LeakSanitizer 보고를
 * 막는다. */
static router_t *volatile g_last_router;

static int conn_setup(conn_fixture_t *fx)
{
    char err[256];
    struct sockaddr_storage peer;
    struct sockaddr_in *in = (struct sockaddr_in *)&peer;

    memset(fx, 0, sizeof(*fx));
    fx->cfg = fake_config();
    if (fx->cfg == NULL || test_write_file(fx->cfg->root, "ok.txt", "ok", 2, 0644) != 0) {
        return -1;
    }
    fx->router = router_new(fx->cfg, NULL, NULL, err, sizeof(err));
    g_last_router = fx->router;
    if (fx->router == NULL) {
        return -1;
    }
    fx->env.cfg = fx->cfg;
    fx->env.lim = &LIM;
    fx->env.router = fx->router;
    fx->env.engine = NULL;
    fx->env.nonblocking = true;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fx->sv) != 0) {
        return -1;
    }
    memset(&peer, 0, sizeof(peer));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fx->c = conn_new(fx->sv[0], &peer, &fx->env, fake_now_ms());
    return fx->c == NULL ? -1 : 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* 이벤트를 주며 conn을 돌린다. stop_on_read면 WANT_READ에서 멈춘다. 최종 상태를 돌려준다. */
static conn_status_t drive(conn_fixture_t *fx, int stop_on_read)
{
    conn_status_t st = conn_on_event(fx->c, fx->sv[0], EV_READ, fake_now_ms());

    for (int i = 0; i < 400 && st != CONN_CLOSE; i++) {
        int64_t dl;
        if (st == CONN_WANT_READ && stop_on_read) {
            break;
        }
        sleep_ms(1);
        fake_advance_ms(10);
        dl = conn_deadline_ms(fx->c);
        if (dl >= 0 && fake_now_ms() >= dl) {
            st = conn_on_timeout(fx->c, fake_now_ms());
        } else {
            st = conn_on_event(fx->c, fx->sv[0], st == CONN_WANT_WRITE ? EV_WRITE : EV_READ,
                               fake_now_ms());
        }
    }
    return st;
}

/* fd에서 timeout_ms 동안 더 오는 것이 없을 때까지 읽는다. */
static size_t read_all(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t n = 0;

    while (n < cap - 1) {
        struct pollfd pfd = {fd, POLLIN, 0};
        ssize_t r;
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            break;
        }
        r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
    }
    buf[n] = '\0';
    return n;
}

static void t_u_ds_40(void)
{
    const char *req = "GET /ok.txt HTTP/1.1\r\nHost: t\r\n\r\n";
    conn_fixture_t fx;
    conn_status_t st;
    char resp[4096];
    char *body;

    CHECK_EQ_INT(conn_setup(&fx), 0);
    CHECK_EQ_INT(conn_fd(fx.c), fx.sv[0]);
    CHECK_EQ_INT(write(fx.sv[1], req, strlen(req)), (long long)strlen(req));
    st = drive(&fx, 1);
    CHECK_EQ_INT(st, CONN_WANT_READ);
    read_all(fx.sv[1], resp, sizeof(resp), 200);
    CHECK(strncmp(resp, "HTTP/1.1 200 ", 13) == 0);
    CHECK(strstr(resp, "\r\nContent-Length: 2\r\n") != NULL);
    body = strstr(resp, "\r\n\r\n");
    CHECK(body != NULL);
    CHECK_EQ_STR(body + 4, "ok");
    conn_free(fx.c);
    close(fx.sv[1]);
}

static void t_u_ds_44(void)
{
    const char *req = "GARBAGE\r\n\r\n";
    conn_fixture_t fx;
    conn_status_t st;
    char resp[4096];

    CHECK_EQ_INT(conn_setup(&fx), 0);
    CHECK_EQ_INT(write(fx.sv[1], req, strlen(req)), (long long)strlen(req));
    CHECK_EQ_INT(shutdown(fx.sv[1], SHUT_WR), 0);
    st = drive(&fx, 0);
    CHECK_EQ_INT(st, CONN_CLOSE);
    read_all(fx.sv[1], resp, sizeof(resp), 200);
    CHECK(strncmp(resp, "HTTP/1.1 400 ", 13) == 0);
    CHECK(strstr(resp, "\r\nConnection: close\r\n") != NULL);
    conn_free(fx.c);
    close(fx.sv[1]);
}

void register_downstream_tests(void)
{
    test_register("U-DS-01", "request_parse_head: simple GET", t_u_ds_01);
    test_register("U-DS-06", "request_parse_head: two pipelined requests", t_u_ds_06);
    test_register("U-DS-07", "request_parse_head: bad token count / double space", t_u_ds_07);
    test_register("U-DS-12", "request_parse_head: Host rules for 1.1 and 1.0", t_u_ds_12);
    test_register("U-DS-22", "uri_decode_path: %20 and UTF-8", t_u_ds_22);
    test_register("U-DS-27", "path_normalize: dot, double slash, dotdot", t_u_ds_27);
    test_register("U-DS-28", "path_normalize: escaping root fails", t_u_ds_28);
    test_register("U-DS-30", "response_write_head: exact 200 head", t_u_ds_30);
    test_register("U-DS-36", "listener_open: port 0, nonblock, cloexec", t_u_ds_36);
    test_register("U-DS-40", "conn: one request, 200 ok, keep-alive", t_u_ds_40);
    test_register("U-DS-44", "conn: broken request, 400 + close", t_u_ds_44);
}
