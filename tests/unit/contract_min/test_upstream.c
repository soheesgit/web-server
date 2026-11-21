/* tests/unit/contract_min/test_upstream.c
 *
 * upstream/ 계약 최소셋. 기대값의 원래 표: 기획서 11.1~11.3절. ID 목록: src/upstream/README.md
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
#include <unistd.h>

#include "common/http_framing.h"
#include "core/buf.h"
#include "fakes/fake_handler.h"
#include "upstream/request_writer.h"
#include "upstream/response_parser.h"
#include "upstream/upstream.h"

static const http_limits_t LIM = {8192, 2048, 64, 10485760};

static int count_of(const char *p, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    int c = 0;

    for (size_t i = 0; p != NULL && i + k <= n; i++) {
        if (memcmp(p + i, needle, k) == 0) {
            c++;
        }
    }
    return c;
}

static config_t *cfg_with_group(int nbackends, uint16_t first_port)
{
    config_t *cfg = fake_config();
    upstream_conf_t *u;

    if (cfg == NULL) {
        return NULL;
    }
    u = &cfg->upstreams[cfg->n_upstreams++];
    (void)snprintf(u->name, sizeof(u->name), "%s", "g");
    for (int i = 0; i < nbackends; i++) {
        (void)snprintf(u->backends[i].host, sizeof(u->backends[i].host), "%s", "127.0.0.1");
        u->backends[i].port = (uint16_t)(first_port + i);
    }
    u->n_backends = (size_t)nbackends;
    return cfg;
}

static void t_u_us_01(void)
{
    http_request_t req;
    upstream_rewrite_t rw;
    buf_t out;
    const char *line = "GET /x HTTP/1.1\r\n";

    fake_request(&req, HTTP_GET, "/x", "a.com");
    memset(&rw, 0, sizeof(rw));
    rw.client_ip.ptr = "127.0.0.1";
    rw.client_ip.len = 9;
    rw.upstream_host.ptr = "127.0.0.1:18001";
    rw.upstream_host.len = 15;

    rw.keep_host = false;
    CHECK_EQ_INT(buf_init(&out, 1024, 1 << 20), 0);
    CHECK_EQ_INT(request_write_upstream(&req, &rw, &out), 0);
    CHECK(out.len > strlen(line));
    CHECK_EQ_MEM(out.data, strlen(line), line, strlen(line));
    CHECK_EQ_INT(count_of(out.data, out.len, "\r\nHost: 127.0.0.1:18001\r\n"), 1);
    CHECK_EQ_INT(count_of(out.data, out.len, "\r\nHost:"), 1);
    buf_cleanup(&out);

    rw.keep_host = true;
    CHECK_EQ_INT(buf_init(&out, 1024, 1 << 20), 0);
    CHECK_EQ_INT(request_write_upstream(&req, &rw, &out), 0);
    CHECK_EQ_INT(count_of(out.data, out.len, "\r\nHost: a.com\r\n"), 1);
    CHECK_EQ_INT(count_of(out.data, out.len, "\r\nHost:"), 1);
    buf_cleanup(&out);
}

static void t_u_us_09(void)
{
    const char *p = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    http_response_t resp;
    int st = 0;
    http_body_tracker_t tracker;
    size_t used;

    memset(&resp, 0, sizeof(resp));
    CHECK_EQ_INT(response_parse_head(NULL, 0, HTTP_GET, &LIM, &resp, &st), HTTP_PARSE_INCOMPLETE);
    for (size_t n = 1; n < strlen(p); n++) {
        CHECK_EQ_INT(response_parse_head(p, n, HTTP_GET, &LIM, &resp, &st), HTTP_PARSE_INCOMPLETE);
    }
    CHECK_EQ_INT(response_parse_head(p, strlen(p), HTTP_GET, &LIM, &resp, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.status, 200);
    CHECK_STR_EQ(resp.reason, "OK");
    CHECK_EQ_INT(resp.minor, 1);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(resp.framing.length, 5);
    CHECK_EQ_INT(resp.head_len, strlen(p));

    int fds[2];
    char body[5];
    CHECK_EQ_INT(pipe(fds), 0);
    CHECK_EQ_INT(write(fds[1], "a\0", 2), 2);
    CHECK_EQ_INT(close(fds[1]), 0);
    CHECK_EQ_INT(read(fds[0], body, sizeof(body)), 2);
    http_body_tracker_init(&tracker, resp.framing);
    CHECK_EQ_INT(http_body_track(&tracker, body, 2, &used), BODY_MORE);
    CHECK_EQ_INT(read(fds[0], body, sizeof(body)), 0);
    CHECK_EQ_INT(close(fds[0]), 0);
    CHECK_EQ_INT(http_body_track(&tracker, NULL, 0, &used), BODY_MORE);
    CHECK_EQ_INT(tracker.remaining, 3);

    CHECK_EQ_INT(response_parse_head(p, strlen(p), HTTP_HEAD, &LIM, &resp, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_NONE);
    const char close_delimited[] = "HTTP/1.1 200\r\n\r\n";
    CHECK_EQ_INT(response_parse_head(close_delimited, sizeof(close_delimited) - 1, HTTP_GET, &LIM,
                                     &resp, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_CLOSE);
    CHECK(!resp.keep_alive);
    http_body_tracker_init(&tracker, resp.framing);
    CHECK_EQ_INT(http_body_track(&tracker, "a\0b", 3, &used), BODY_MORE);
    CHECK_EQ_INT(used, 3);

    const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    CHECK_EQ_INT(response_parse_head(chunked, sizeof(chunked) - 1, HTTP_GET, &LIM, &resp, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_CHUNKED);
    CHECK_EQ_INT(resp.headers.n, 2);
    CHECK(!resp.keep_alive);
    http_body_tracker_init(&tracker, resp.framing);
    CHECK_EQ_INT(http_body_track(&tracker, "3\r\nxy", 5, &used), BODY_MORE);
    CHECK_EQ_INT(http_body_track(&tracker, NULL, 0, &used), BODY_MORE);

    const char informational[] = "\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204\r\n\r\n";
    CHECK_EQ_INT(
        response_parse_head(informational, sizeof(informational) - 1, HTTP_GET, &LIM, &resp, &st),
        HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.status, 100);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_NONE);
    size_t first = resp.head_len;
    CHECK_EQ_INT(response_parse_head(informational + first, sizeof(informational) - 1 - first,
                                     HTTP_GET, &LIM, &resp, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.status, 204);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_NONE);

    const char keep10[] = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
    CHECK_EQ_INT(response_parse_head(keep10, sizeof(keep10) - 1, HTTP_GET, &LIM, &resp, &st),
                 HTTP_PARSE_DONE);
    CHECK(resp.keep_alive);
}

static void t_u_us_12(void)
{
    static const char *bad[] = {
        "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n",
        "FOO/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 99 X\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX-Data : a\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
    };
    const char *good = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    http_response_t resp;
    int st = 0;

    /* 정상 대조 (D-017) */
    CHECK_EQ_INT(response_parse_head(good, strlen(good), HTTP_GET, &LIM, &resp, &st),
                 HTTP_PARSE_DONE);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (response_parse_head(bad[i], strlen(bad[i]), HTTP_GET, &LIM, &resp, &st) !=
                HTTP_PARSE_ERROR ||
            st != 502) {
            test_fail(__FILE__, __LINE__, "case %zu should be ERROR", i);
            return;
        }
    }
}

static void t_u_us_17(void)
{
    static const char *want[] = {"127.0.0.1:18001", "127.0.0.1:18002", "127.0.0.1:18003"};
    config_t *cfg = cfg_with_group(3, 18001);
    upstream_registry_t *reg;
    upstream_group_t *g;
    char err[256] = "";

    CHECK(cfg != NULL);
    reg = upstream_registry_new(cfg, err, sizeof(err));
    CHECK(reg != NULL);
    g = upstream_registry_get(reg, "g");
    CHECK(g != NULL);
    for (int i = 0; i < 9; i++) {
        backend_t *b = upstream_pick(g, NULL, fake_now_ms());
        CHECK(b != NULL);
        CHECK_EQ_STR(backend_hostport(b), want[i % 3]);
    }
    upstream_registry_free(reg);
}

static void t_u_us_24(void)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    config_t *cfg;
    upstream_registry_t *reg;
    upstream_group_t *g;
    backend_t *b;
    upstream_conn_t *c = NULL, *c2 = NULL;
    char err[256] = "";
    int sfd, fd1;

    CHECK(lfd >= 0);
    CHECK_EQ_INT(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ_INT(bind(lfd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    CHECK_EQ_INT(listen(lfd, 8), 0);
    CHECK_EQ_INT(getsockname(lfd, (struct sockaddr *)&sa, &sl), 0);

    cfg = cfg_with_group(1, ntohs(sa.sin_port));
    CHECK(cfg != NULL);
    reg = upstream_registry_new(cfg, err, sizeof(err));
    CHECK(reg != NULL);
    g = upstream_registry_get(reg, "g");
    CHECK(g != NULL);
    b = upstream_pick(g, NULL, fake_now_ms());
    CHECK(b != NULL);

    CHECK_EQ_INT(upstream_acquire(g, b, &c), 0);
    CHECK(c != NULL);
    CHECK(c->reused == false);
    if (c->connecting) {
        struct pollfd pfd = {c->fd, POLLOUT, 0};
        CHECK_EQ_INT(poll(&pfd, 1, 2000), 1);
    }
    CHECK_EQ_INT(upstream_connect_result(c), 0);
    sfd = accept(lfd, NULL, NULL); /* 백엔드 쪽은 연결을 열어 둔다 */
    CHECK(sfd >= 0);
    fd1 = c->fd;
    upstream_release(c, true);

    CHECK(fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK) == 0);
    CHECK_EQ_INT(upstream_acquire(g, b, &c2), 0);
    CHECK(c2 != NULL);
    CHECK_EQ_INT(c2->fd, fd1);
    CHECK(c2->reused == true);
    errno = 0;
    CHECK(accept(lfd, NULL, NULL) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    upstream_release(c2, false);
    upstream_registry_free(reg);
    close(sfd);
    close(lfd);
}

void register_upstream_tests(void)
{
    test_register("U-US-01", "request_write_upstream: Host rewrite / keep_host", t_u_us_01);
    test_register("U-US-09", "response_parse_head: 200 OK with CL 5", t_u_us_09);
    test_register("U-US-12", "response_parse_head: bad status lines are ERROR", t_u_us_12);
    test_register("U-US-17", "upstream_pick: round robin A B C x3", t_u_us_17);
    test_register("U-US-24", "upstream_release(reusable) then acquire reuses fd", t_u_us_24);
}
