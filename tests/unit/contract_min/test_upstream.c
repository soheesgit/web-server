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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/buf.h"
#include "fakes/fake_handler.h"
#include "upstream/relay.h"
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

    memset(&resp, 0, sizeof(resp));
    CHECK_EQ_INT(response_parse_head(p, strlen(p), HTTP_GET, &LIM, &resp, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(resp.status, 200);
    CHECK_STR_EQ(resp.reason, "OK");
    CHECK_EQ_INT(resp.minor, 1);
    CHECK_EQ_INT(resp.framing.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(resp.framing.length, 5);
    CHECK_EQ_INT(resp.head_len, strlen(p));
}

static void t_u_us_12(void)
{
    static const char *bad[] = {
        "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n",
        "FOO/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 99 X\r\nContent-Length: 0\r\n\r\n",
    };
    const char *good = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    http_response_t resp;
    int st = 0;

    /* 정상 대조 (D-017) */
    CHECK_EQ_INT(response_parse_head(good, strlen(good), HTTP_GET, &LIM, &resp, &st),
                 HTTP_PARSE_DONE);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (response_parse_head(bad[i], strlen(bad[i]), HTTP_GET, &LIM, &resp, &st) !=
            HTTP_PARSE_ERROR) {
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

/* U-US-29 백엔드: 첫 요청에는 응답하고, 같은 연결의 둘째 요청은 받기만 하고 응답 없이 닫는다.
 * 그 뒤 새 연결이 오면(재전송) 응답한다. */
typedef struct {
    int lfd;
    int accepted;
    int requests;
} drop_backend_t;

static int read_head(int fd)
{
    char buf[2048];
    size_t n = 0;
    while (n < sizeof(buf)) {
        struct pollfd p = {fd, POLLIN, 0};
        ssize_t r;
        if (poll(&p, 1, 2000) != 1 || (r = read(fd, buf + n, sizeof(buf) - n)) <= 0) {
            return -1;
        }
        n += (size_t)r;
        for (size_t i = 0; i + 3 < n; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static void *drop_backend(void *arg)
{
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    drop_backend_t *d = arg;
    struct pollfd p = {d->lfd, POLLIN, 0};
    int a = accept(d->lfd, NULL, NULL);
    if (a < 0) {
        return NULL;
    }
    d->accepted++;
    if (read_head(a) == 0) {
        d->requests++;
        if (write(a, ok, sizeof(ok) - 1) < 0) {
            d->requests = -100; /* 쓰기 실패는 판정에서 드러나게 */
        }
    }
    if (read_head(a) == 0) {
        d->requests++;
    }
    close(a);
    if (poll(&p, 1, 500) == 1) {
        int b = accept(d->lfd, NULL, NULL);
        if (b >= 0) {
            d->accepted++;
            if (read_head(b) == 0) {
                d->requests++;
                if (write(b, ok, sizeof(ok) - 1) < 0) {
                    d->requests = -100; /* 쓰기 실패는 판정에서 드러나게 */
                }
            }
            close(b);
        }
    }
    return NULL;
}

/* 연결 하나를 풀에 데워 둔 뒤, 그 재사용 연결로 method 요청을 보낸 결과를 돌려준다. */
static void drop_exchange(http_method_t method, drop_backend_t *d, proxy_fail_t *fail,
                          relay_status_t *out_st)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int one = 1;
    char err[256] = "";
    config_t *cfg;
    upstream_registry_t *reg;
    proxy_job_t job;
    http_response_t resp;
    upstream_conn_t *uc = NULL;
    pthread_t th;
    static const char get[] = "GET /x HTTP/1.1\r\nHost: b\r\n\r\n";
    static const char post[] = "POST /x HTTP/1.1\r\nHost: b\r\nContent-Length: 0\r\n\r\n";

    memset(d, 0, sizeof(*d));
    d->lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(d->lfd >= 0);
    CHECK_EQ_INT(setsockopt(d->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ_INT(bind(d->lfd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    CHECK_EQ_INT(listen(d->lfd, 8), 0);
    CHECK_EQ_INT(getsockname(d->lfd, (struct sockaddr *)&sa, &sl), 0);
    cfg = cfg_with_group(1, ntohs(sa.sin_port));
    CHECK(cfg != NULL);
    reg = upstream_registry_new(cfg, err, sizeof(err));
    CHECK(reg != NULL);
    CHECK_EQ_INT(pthread_create(&th, NULL, drop_backend, d), 0);

    memset(&job, 0, sizeof(job));
    job.group = upstream_registry_get(reg, "g");
    job.backend = upstream_pick(job.group, NULL, fake_now_ms());
    job.attempts = job.max_attempts = 1;
    CHECK_EQ_INT(buf_init(&job.request, 256, 4096), 0);
    /* 1) 첫 요청: 정상 응답을 받고 연결을 풀에 돌려준다 */
    job.method = HTTP_GET;
    CHECK_EQ_INT(buf_append(&job.request, get, sizeof(get) - 1), 0);
    CHECK_EQ_INT(relay_exchange_blocking(&job, &LIM, 1000, 1000, &uc, &resp, fail), RELAY_OK);
    buf_consume(&uc->rbuf, uc->rbuf.len);
    upstream_release(uc, true);
    /* 2) 같은 연결로 둘째 요청: 백엔드가 받기만 하고 닫는다 */
    uc = NULL;
    buf_reset(&job.request);
    job.method = method;
    if (method == HTTP_POST) {
        CHECK_EQ_INT(buf_append(&job.request, post, sizeof(post) - 1), 0);
    } else {
        CHECK_EQ_INT(buf_append(&job.request, get, sizeof(get) - 1), 0);
    }
    *out_st = relay_exchange_blocking(&job, &LIM, 1000, 1000, &uc, &resp, fail);
    if (uc != NULL) {
        upstream_release(uc, false);
    }
    pthread_join(th, NULL);
    buf_cleanup(&job.request);
    upstream_registry_free(reg);
    free(cfg); /* 한 테스트에서 두 번 만들므로 직접 해제한다 */
    close(d->lfd);
}

static void t_u_us_29(void)
{
    drop_backend_t d;
    proxy_fail_t fail = PFAIL_CONNECT;
    relay_status_t st = RELAY_FAIL;

    /* GET은 멱등이라 새 연결로 한 번 다시 보내 응답을 받는다 (D-036) */
    drop_exchange(HTTP_GET, &d, &fail, &st);
    CHECK_EQ_INT(st, RELAY_OK);
    CHECK_EQ_INT(d.accepted, 2);
    CHECK_EQ_INT(d.requests, 3);
    /* POST는 백엔드가 이미 처리했을 수 있어 다시 보내지 않는다 */
    drop_exchange(HTTP_POST, &d, &fail, &st);
    CHECK_EQ_INT(st, RELAY_FAIL);
    CHECK_EQ_INT(fail, PFAIL_BAD_RESPONSE);
    CHECK_EQ_INT(d.accepted, 1);
    CHECK_EQ_INT(d.requests, 2);
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
    test_register("U-US-29", "relay: reused conn dropped before reply, GET retried, POST not",
                  t_u_us_29);
}
